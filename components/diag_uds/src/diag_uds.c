#include "diag_uds.h"
#include "diag_uds_private.h"
#include "can_if.h"
#include "iso14229.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(diag_uds);

#define P2_MS 50U
#define RESPONSE_PACING_MS 1U
#define P2_STAR_MS 5000U
#define S3_MS 5100U
#define HANDLER_CAPACITY 128U
#define MAX_ROUTINE_INPUT 64U
#define MAX_DIDS_PER_REQUEST 32U

struct did_handler {
	uint16_t id;
	diag_uds_did_handler_t fn;
	void *context;
};
struct routine_handler {
	uint16_t id;
	diag_uds_routine_handler_t fn;
	void *context;
};
static struct did_handler dids[CONFIG_DIAG_UDS_MAX_HANDLERS];
static struct routine_handler routines[CONFIG_DIAG_UDS_MAX_HANDLERS];
static size_t did_count, routine_count;
static bool initialized, started;
static int startup_result;
K_MUTEX_DEFINE(diag_lifecycle_lock);
K_SEM_DEFINE(diag_worker_ready, 0, 1);
K_MSGQ_DEFINE(diag_rx_queue, sizeof(can_if_frame_t), CONFIG_DIAG_UDS_RX_QUEUE_SIZE, 4);

#define COUNTERS(X) \
	X(rx_queued) X(rx_dropped) X(rx_invalid) X(rx_busy) X(requests) X(rejected) \
	X(responses_started) X(suppressed) X(transport_errors) X(handler_errors) \
	X(session_timeouts) X(poll_count) X(max_poll_gap_us) X(poll_deadline_misses)
#define DECLARE_COUNTER(name) static atomic_t name;
COUNTERS(DECLARE_COUNTER)
static atomic_t session = ATOMIC_INIT(1);

/* All following protocol/server/response state belongs exclusively to the worker. */
static UDSServer_t server;
static UDSTpISOTpC_t transport;
static UDSTp_t admitted_transport;
static uint8_t handler_output[HANDLER_CAPACITY];
static struct {
	uint8_t data[UDS_SERVER_SEND_BUF_SIZE];
	size_t length;
	uint32_t deadline;
} response;
static bool functional_request;
static uint32_t request_time;
static int last_receive_result;

static bool suppressed_nrc(uint8_t nrc)
{
	return nrc == 0x11 || nrc == 0x12 || nrc == 0x31 || nrc == 0x7e || nrc == 0x7f;
}

static UDSErr_t enqueue_response(const uint8_t *data, size_t length)
{
	if (functional_request && length == 3 && data[0] == 0x7f &&
	    suppressed_nrc(data[2])) {
		atomic_inc(&suppressed);
		return UDS_OK;
	}
	if (response.length != 0 || length > sizeof(response.data)) {
		return UDS_ERR_BUFSIZ;
	}
	memcpy(response.data, data, length);
	response.length = length;
	response.deadline = request_time + P2_MS;
	return UDS_OK;
}

static UDSErr_t send_response(UDSTp_t *hdl, const uint8_t *data, size_t length,
			      const UDSSDU_t *info)
{
	ARG_UNUSED(hdl);
	ARG_UNUSED(info);
	/* A bounded owned copy, retried only until the original request's P2 limit. */
	return enqueue_response(data, length);
}

static uint8_t admit_request(const uint8_t *data, size_t length)
{
	switch (data[0]) {
	case 0x10:
	case 0x3e:
		return length == 2 ? 0 : 0x13;
	case 0x22:
		return length >= 3 && (length % 2) == 1 &&
		       length <= 1 + 2 * MAX_DIDS_PER_REQUEST ? 0 : 0x13;
	case 0x31:
		if (functional_request) {
			return 0x11; /* Mutating routines are physical-only. */
		}
		return length >= 4 && length <= 4 + MAX_ROUTINE_INPUT ? 0 : 0x13;
	default:
		return 0x11;
	}
}

static UDSErr_t receive_request(UDSTp_t *hdl, uint8_t *data, size_t capacity,
				size_t *length, UDSSDU_t *info)
{
	ARG_UNUSED(hdl);
	*length = 0;
	if (response.length != 0 || transport.phys_link.send_status == ISOTP_SEND_STATUS_INPROGRESS) {
		return UDS_OK;
	}
	UDSErr_t ret = transport.hdl.recv(&transport.hdl, data, capacity, length, info);

	if (ret != UDS_OK || *length == 0) {
		return ret;
	}
	functional_request = info->A_TA_Type == UDS_A_TA_TYPE_FUNCTIONAL;
	request_time = UDSMillis();
	atomic_inc(&requests);
	uint8_t nrc = admit_request(data, *length);

	if (nrc != 0) {
		uint8_t negative[] = {0x7f, data[0], nrc};

		atomic_inc(&rejected);
		ret = enqueue_response(negative, sizeof(negative));
		*length = 0;
	} else if (server.sessionType != 1) {
		/* Every admitted request restarts S3, including suppressed positives. */
		server.s3_session_timeout_timer = request_time + S3_MS;
	}
	return ret;
}

static UDSErr_t poll_transport(UDSTp_t *hdl)
{
	UDSErr_t ret = transport.hdl.poll(&transport.hdl);

	hdl->status.is_sending = transport.hdl.status.is_sending || response.length != 0;
	if (ret != UDS_OK) {
		atomic_inc(&transport_errors);
		/* Abort failed output so a subsequent tester request can recover. */
		transport.phys_link.send_status = ISOTP_SEND_STATUS_IDLE;
	}
	int result = transport.phys_link.receive_protocol_result;

	if (result != ISOTP_PROTOCOL_RESULT_OK && result != last_receive_result) {
		atomic_inc(&transport_errors);
	}
	last_receive_result = result;
	return ret;
}

static UDSErr_t handler_result(diag_uds_result_t result, size_t length)
{
	if (result == DIAG_UDS_OK) {
		if (length > HANDLER_CAPACITY) {
			atomic_inc(&handler_errors);
			return UDS_NRC_ResponseTooLong;
		}
		return UDS_PositiveResponse;
	}
	switch (result) {
	case DIAG_UDS_GENERAL_REJECT:
	case DIAG_UDS_SUBFUNCTION_NOT_SUPPORTED:
	case DIAG_UDS_INVALID_LENGTH:
	case DIAG_UDS_RESPONSE_TOO_LONG:
	case DIAG_UDS_CONDITIONS_NOT_CORRECT:
	case DIAG_UDS_REQUEST_OUT_OF_RANGE:
		return (UDSErr_t)result;
	default:
		/* No asynchronous/ResponsePending contract is exposed. */
		atomic_inc(&handler_errors);
		return UDS_NRC_GeneralReject;
	}
}

static UDSErr_t server_event(UDSServer_t *srv, UDSEvent_t event, void *arg)
{
	atomic_set(&session, srv->sessionType);
	switch (event) {
	case UDS_EVT_DiagSessCtrl: {
		UDSDiagSessCtrlArgs_t *args = arg;

		if (args->type != 1 && args->type != 3) {
			return UDS_NRC_SubFunctionNotSupported;
		}
		args->p2_ms = P2_MS;
		args->p2_star_ms = P2_STAR_MS;
		return UDS_PositiveResponse;
	}
	case UDS_EVT_ReadDataByIdent: {
		UDSRDBIArgs_t *args = arg;

		for (size_t i = 0; i < did_count; ++i) {
			if (dids[i].id != args->dataId) {
				continue;
			}
			size_t length = 0;
			diag_uds_result_t result = dids[i].fn(handler_output,
				HANDLER_CAPACITY, &length, dids[i].context);
			UDSErr_t ret = handler_result(result, length);
			if (ret != UDS_PositiveResponse) {
				return ret;
			}
			return length > 0 ? args->copy(srv, handler_output, (uint16_t)length) :
				UDS_NRC_GeneralReject;
		}
		return UDS_NRC_RequestOutOfRange;
	}
	case UDS_EVT_RoutineCtrl: {
		UDSRoutineCtrlArgs_t *args = arg;

		for (size_t i = 0; i < routine_count; ++i) {
			if (routines[i].id != args->id) {
				continue;
			}
			size_t length = 0;
			diag_uds_result_t result = routines[i].fn(args->ctrlType,
				args->optionRecord, args->len, handler_output, HANDLER_CAPACITY,
				&length, routines[i].context);
			UDSErr_t ret = handler_result(result, length);

			return ret == UDS_PositiveResponse ?
				args->copyStatusRecord(srv, handler_output, (uint16_t)length) : ret;
		}
		return UDS_NRC_RequestOutOfRange;
	}
	case UDS_EVT_SessionTimeout:
		atomic_inc(&session_timeouts);
		return UDS_OK;
	case UDS_EVT_Err:
		atomic_inc(&transport_errors);
		return UDS_OK;
	default:
		return UDS_NRC_ServiceNotSupported;
	}
}

static void flush_response(void)
{
	if (response.length == 0) {
		return;
	}
	if (UDSTimeAfter(UDSMillis(), response.deadline)) {
		atomic_inc(&transport_errors);
		response.length = 0;
		return;
	}
	can_if_stats_t stats;

	can_if_get_stats(&stats);
	if (stats.tx_pending || transport.phys_link.send_status == ISOTP_SEND_STATUS_INPROGRESS) {
		return;
	}
	UDSErr_t ret = transport.hdl.send(&transport.hdl, response.data, response.length, NULL);

	if (ret == UDS_OK) {
		atomic_inc(&responses_started);
		response.length = 0;
	} else if (diag_port_last_submit() != -EAGAIN) {
		atomic_inc(&transport_errors);
		response.length = 0;
	}
}

static void receive_frame(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(context);
	/* Dispatcher thread: copy only. No transport or server state access here. */
	if (k_msgq_put(&diag_rx_queue, frame, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	} else {
		atomic_inc(&rx_queued);
	}
}

static void consume_frame(const can_if_frame_t *frame)
{
	bool functional = frame->id == CONFIG_DIAG_UDS_FUNCTIONAL_RX_ID;
	uint8_t type = frame->data[0] >> 4;
	uint8_t sf_length = frame->data[0] & 0x0f;

	if (frame->extended || frame->dlc < 2 || type > 3 ||
	    (functional && type != 0) ||
	    (type == 0 && (sf_length == 0 || sf_length > 7 || sf_length + 1 > frame->dlc)) ||
	    (type == 1 && (frame->dlc != 8 ||
		(((frame->data[0] & 0x0f) << 8) | frame->data[1]) <= 7)) ||
	    (type == 3 && frame->dlc < 3)) {
		atomic_inc(&rx_invalid);
		return;
	}
	IsoTpLink *link = functional ? &transport.func_link : &transport.phys_link;

	if (type != 3 && link->receive_status == ISOTP_RECEIVE_STATUS_FULL) {
		atomic_inc(&rx_busy);
		return;
	}
	isotp_on_can_message(link, frame->data, frame->dlc);
}

static void worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	startup_result = UDSServerTpISOTpCInit(&transport, CONFIG_DIAG_UDS_PHYSICAL_RX_ID,
		CONFIG_DIAG_UDS_PHYSICAL_TX_ID, CONFIG_DIAG_UDS_FUNCTIONAL_RX_ID);
	if (startup_result == UDS_OK) {
		startup_result = UDSServerInit(&server);
	}
	if (startup_result != UDS_OK) {
		startup_result = -EIO;
		k_sem_give(&diag_worker_ready);
		return;
	}
	admitted_transport = (UDSTp_t){.send = send_response, .recv = receive_request,
		.poll = poll_transport};
	server.tp = &admitted_transport;
	server.fn = server_event;
	/* Upstream rearms p2_timer after sending: keep its internal pacing short.
	 * Advertised P2 and the request deadline remain independent at 50 ms.
	 */
	server.p2_ms = RESPONSE_PACING_MS;
	server.p2_star_ms = P2_STAR_MS;
	server.s3_ms = S3_MS;
	server.p2_timer = UDSMillis();
	k_sem_give(&diag_worker_ready);
	LOG_INF("UDS ready: RX %x / TX %x / functional %x",
		CONFIG_DIAG_UDS_PHYSICAL_RX_ID, CONFIG_DIAG_UDS_PHYSICAL_TX_ID,
		CONFIG_DIAG_UDS_FUNCTIONAL_RX_ID);
	uint32_t previous = isotp_user_get_us();
	uint32_t completion_errors = (uint32_t)atomic_get(&diag_tx_completion_errors);

	while (true) {
		uint32_t now = isotp_user_get_us();
		uint32_t gap = now - previous;

		previous = now;
		if (gap > (uint32_t)atomic_get(&max_poll_gap_us)) {
			atomic_set(&max_poll_gap_us, gap);
		}
		if (gap >= 5000) {
			atomic_inc(&poll_deadline_misses);
		}
		uint32_t errors = (uint32_t)atomic_get(&diag_tx_completion_errors);

		if (errors != completion_errors) {
			/* Driver callbacks only publish counters; protocol abort stays here. */
			completion_errors = errors;
			atomic_inc(&transport_errors);
			transport.phys_link.send_status = ISOTP_SEND_STATUS_IDLE;
			transport.phys_link.receive_status = ISOTP_RECEIVE_STATUS_IDLE;
			server.requestInProgress = false;
			response.length = 0;
			diag_port_abort();
		}
		diag_port_poll();
		for (size_t i = 0; i < 8 && !diag_port_fc_pending(); ++i) {
			can_if_frame_t frame;

			if (k_msgq_get(&diag_rx_queue, &frame, K_NO_WAIT) != 0) {
				break;
			}
			consume_frame(&frame);
		}
		UDSServerPoll(&server);
		atomic_set(&session, server.sessionType);
		atomic_inc(&poll_count);
		if (!diag_port_fc_pending()) {
			flush_response();
		}
		k_sleep(K_MSEC(CONFIG_DIAG_UDS_POLL_MS));
	}
}

K_THREAD_DEFINE(worker_id, CONFIG_DIAG_UDS_STACK_SIZE, worker, NULL, NULL, NULL,
		CONFIG_DIAG_UDS_PRIORITY, 0, SYS_FOREVER_MS);

int diag_uds_init(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_mutex_lock(&diag_lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret = 0;

	if (initialized) {
		ret = -EALREADY;
	} else if (CONFIG_DIAG_UDS_PHYSICAL_RX_ID == CONFIG_DIAG_UDS_FUNCTIONAL_RX_ID ||
		   CONFIG_DIAG_UDS_PHYSICAL_RX_ID == CONFIG_DIAG_UDS_PHYSICAL_TX_ID ||
		   CONFIG_DIAG_UDS_FUNCTIONAL_RX_ID == CONFIG_DIAG_UDS_PHYSICAL_TX_ID) {
		ret = -EINVAL;
	} else {
		ret = can_if_register_rx(CONFIG_DIAG_UDS_PHYSICAL_RX_ID, false, receive_frame, NULL);
		if (ret == 0 || ret == -EALREADY) {
			ret = can_if_register_rx(CONFIG_DIAG_UDS_FUNCTIONAL_RX_ID,
				false, receive_frame, NULL);
		}
		if (ret == 0 || ret == -EALREADY) {
			initialized = true;
			ret = 0;
		}
	}
	k_mutex_unlock(&diag_lifecycle_lock);
	return ret;
}

int diag_uds_register_did(uint16_t did, diag_uds_did_handler_t handler, void *context)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (handler == NULL) {
		return -EINVAL;
	}
	if (k_mutex_lock(&diag_lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret = initialized ? 0 : -EACCES;

	if (started) {
		ret = -EBUSY;
	}
	for (size_t i = 0; ret == 0 && i < did_count; ++i) {
		if (dids[i].id == did) {
			ret = -EALREADY;
		}
	}
	if (ret == 0) {
		if (did_count == ARRAY_SIZE(dids)) {
			ret = -ENOSPC;
		} else {
			dids[did_count++] = (struct did_handler){did, handler, context};
		}
	}
	k_mutex_unlock(&diag_lifecycle_lock);
	return ret;
}

int diag_uds_register_routine(uint16_t rid, diag_uds_routine_handler_t handler, void *context)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (handler == NULL) {
		return -EINVAL;
	}
	if (k_mutex_lock(&diag_lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret = initialized ? 0 : -EACCES;

	if (started) {
		ret = -EBUSY;
	}
	for (size_t i = 0; ret == 0 && i < routine_count; ++i) {
		if (routines[i].id == rid) {
			ret = -EALREADY;
		}
	}
	if (ret == 0) {
		if (routine_count == ARRAY_SIZE(routines)) {
			ret = -ENOSPC;
		} else {
			routines[routine_count++] = (struct routine_handler){rid, handler, context};
		}
	}
	k_mutex_unlock(&diag_lifecycle_lock);
	return ret;
}

int diag_uds_start(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_mutex_lock(&diag_lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret;
	can_if_stats_t stats;

	can_if_get_stats(&stats);
	if (!initialized || stats.state == CAN_IF_STOPPED) {
		ret = -EACCES;
	} else if (started) {
		ret = -EALREADY;
	} else {
		started = true;
		k_thread_start(worker_id);
		ret = k_sem_take(&diag_worker_ready, K_MSEC(100)) == 0 ? startup_result : -ETIMEDOUT;
	}
	k_mutex_unlock(&diag_lifecycle_lock);
	return ret;
}

int diag_uds_get_stats(diag_uds_stats_t *stats)
{
	if (stats == NULL) {
		return -EINVAL;
	}
#define READ_COUNTER(name) stats->name = (uint32_t)atomic_get(&name);
	COUNTERS(READ_COUNTER)
	stats->tx_submit_errors = (uint32_t)atomic_get(&diag_tx_submit_errors);
	stats->tx_busy = (uint32_t)atomic_get(&diag_tx_busy);
	stats->tx_completed = (uint32_t)atomic_get(&diag_tx_completed);
	stats->tx_completion_errors = (uint32_t)atomic_get(&diag_tx_completion_errors);
	stats->last_tx_error = (int)atomic_get(&diag_last_tx_error);
	stats->session = (uint8_t)atomic_get(&session);
	return 0;
}
