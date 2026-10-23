#include "can_if.h"
#include "diag_uds.h"
#include "diag_uds_private.h"
#include "iso14229.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;
K_MSGQ_DEFINE(sent_frames, sizeof(struct can_frame), 32, 4);
K_SEM_DEFINE(handler_entered, 0, 1);
K_SEM_DEFINE(handler_release, 0, 1);
static const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static can_rx_callback_t ingress;
static int send_failure;
static int completion_failure;
static bool oversized_result;
static struct can_frame injected;
static atomic_t handler_context_errors;

static int capture(const struct device *device, const struct can_frame *frame,
		   k_timeout_t timeout, can_tx_callback_t callback, void *context)
{
	zassert_true(K_TIMEOUT_EQ(timeout, K_NO_WAIT));
	if (send_failure != 0) {
		return send_failure;
	}
	zassert_ok(k_msgq_put(&sent_frames, frame, K_NO_WAIT));
	callback(device, completion_failure, context);
	return 0;
}

static diag_uds_result_t read_value(uint8_t *out, size_t capacity,
				  size_t *length, void *context)
{
	ARG_UNUSED(context);
	if (k_is_in_isr()) { atomic_inc(&handler_context_errors); }
	zassert_true(capacity >= 2);
	out[0] = 0x11;
	out[1] = 0x22;
	*length = oversized_result ? capacity + 1 : 2;
	return DIAG_UDS_OK;
}

static diag_uds_result_t blocked_value(uint8_t *out, size_t capacity,
				     size_t *length, void *context)
{
	ARG_UNUSED(out);
	ARG_UNUSED(capacity);
	ARG_UNUSED(length);
	ARG_UNUSED(context);
	k_sem_give(&handler_entered);
	k_sem_take(&handler_release, K_FOREVER); /* Test-only backpressure. */
	return DIAG_UDS_CONDITIONS_NOT_CORRECT;
}

static diag_uds_result_t routine(uint8_t control, const uint8_t *input,
	size_t input_length, uint8_t *out, size_t capacity, size_t *length, void *context)
{
	ARG_UNUSED(input);
	ARG_UNUSED(out);
	ARG_UNUSED(capacity);
	ARG_UNUSED(length);
	ARG_UNUSED(context);
	if (control == 3) { return (diag_uds_result_t)0x78; } /* Unsupported async result. */
	return input_length == 0 ? DIAG_UDS_OK : DIAG_UDS_INVALID_LENGTH;
}

static diag_uds_stats_t stats(void)
{
	diag_uds_stats_t value;

	zassert_ok(diag_uds_get_stats(&value));
	return value;
}

static void inject_irq(const void *unused)
{
	ARG_UNUSED(unused);
	ingress(dev, &injected, NULL);
}

static void inject(const uint8_t *data, size_t length)
{
	injected = (struct can_frame){.id = CONFIG_DIAG_UDS_PHYSICAL_RX_ID,
		.dlc = (uint8_t)(length + 1)};
	injected.data[0] = (uint8_t)length;
	memcpy(injected.data + 1, data, length);
	irq_offload(inject_irq, NULL);
	memset(&injected, 0xa5, sizeof(injected));
}

static void expect(const uint8_t *data, size_t length)
{
	struct can_frame frame;

	zassert_ok(k_msgq_get(&sent_frames, &frame, K_MSEC(100)));
	zassert_equal(frame.id, CONFIG_DIAG_UDS_PHYSICAL_TX_ID);
	zassert_equal(frame.data[0], length);
	zassert_mem_equal(frame.data + 1, data, length);
}

static void lifecycle_and_port(void)
{
	zassert_equal(diag_uds_start(), -EACCES);
	zassert_equal(diag_uds_init(), -EACCES);
	zassert_ok(can_if_init());
	zassert_ok(diag_uds_init());
	zassert_equal(diag_uds_init(), -EALREADY);
	zassert_equal(diag_uds_register_did(1, NULL, NULL), -EINVAL);
	zassert_ok(diag_uds_register_did(1, read_value, NULL));
	zassert_equal(diag_uds_register_did(1, read_value, NULL), -EALREADY);
	zassert_ok(diag_uds_register_did(2, blocked_value, NULL));
	zassert_equal(diag_uds_register_did(3, read_value, NULL), -ENOSPC);
	zassert_ok(diag_uds_register_routine(1, routine, NULL));
	zassert_equal(diag_uds_register_routine(1, routine, NULL), -EALREADY);
	zassert_ok(diag_uds_register_routine(2, routine, NULL));
	zassert_equal(diag_uds_register_routine(3, routine, NULL), -ENOSPC);
	zassert_equal(diag_uds_start(), -EACCES);
	zassert_ok(can_if_start());
	ingress = fake_can_add_rx_filter_fake.arg1_val;
	fake_can_send_fake.custom_fake = capture;
	uint8_t data[] = {0x02, 0x7e, 0x00};

	send_failure = -EIO;
	zassert_equal(isotp_user_send_can(CONFIG_DIAG_UDS_PHYSICAL_TX_ID, data, 3, NULL),
		ISOTP_RET_ERROR);
	zassert_equal(stats().last_tx_error, -EIO);
	send_failure = -EAGAIN;
	zassert_equal(isotp_user_send_can(CONFIG_DIAG_UDS_PHYSICAL_TX_ID, data, 3, NULL),
		ISOTP_RET_NOSPACE);
	data[0] = 0x30;
	zassert_equal(isotp_user_send_can(CONFIG_DIAG_UDS_PHYSICAL_TX_ID, data, 3, NULL),
		ISOTP_RET_NOSPACE);
	zassert_true(diag_port_fc_pending());
	send_failure = 0;
	diag_port_poll();
	zassert_false(diag_port_fc_pending());
	send_failure = -EAGAIN;
	zassert_equal(isotp_user_send_can(CONFIG_DIAG_UDS_PHYSICAL_TX_ID, data, 3, NULL),
		ISOTP_RET_NOSPACE);
	k_sleep(K_MSEC(101));
	diag_port_poll();
	zassert_false(diag_port_fc_pending());
	zassert_equal(stats().last_tx_error, -ETIMEDOUT);
	send_failure = 0;
	completion_failure = -ENETUNREACH;
	data[0] = 0x02;
	zassert_equal(isotp_user_send_can(CONFIG_DIAG_UDS_PHYSICAL_TX_ID, data, 3, NULL),
		ISOTP_RET_OK);
	zassert_equal(stats().tx_completion_errors, 1);
	zassert_equal(stats().last_tx_error, -ENETUNREACH);
	completion_failure = 0;
	zassert_equal(isotp_user_send_can(0, data, 3, NULL), ISOTP_RET_ERROR);
	zassert_equal(isotp_user_send_can(CONFIG_DIAG_UDS_PHYSICAL_TX_ID, data, 9, NULL),
		ISOTP_RET_ERROR);
	k_msgq_purge(&sent_frames);
	uint32_t before = isotp_user_get_us();

	k_sleep(K_MSEC(3));
	zassert_true((uint32_t)(isotp_user_get_us() - before) >= 3000);
	zassert_equal(isotp_user_get_us(), (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks()));
	zassert_equal(diag_port_us_from_ticks(k_ms_to_ticks_ceil64(4294967)), 4294967000U);
	zassert_equal(diag_port_us_from_ticks(k_ms_to_ticks_ceil64(4294968)), 704U);
	zassert_true(UDSTimeAfter(1, UINT32_MAX - 1));
	zassert_true(IsoTpTimeAfter(704, 4294967000U));
	zassert_ok(diag_uds_start());
	zassert_equal(diag_uds_start(), -EALREADY);
	zassert_equal(diag_uds_register_did(1, read_value, NULL), -EBUSY);
	zassert_equal(diag_uds_register_routine(1, routine, NULL), -EBUSY);
}

static void queue_overflow(void)
{
	uint8_t request[] = {0x22, 0, 2};

	inject(request, sizeof(request));
	zassert_ok(k_sem_take(&handler_entered, K_MSEC(100)));
	uint32_t queued = stats().rx_queued;

	for (size_t i = 0; i < CONFIG_DIAG_UDS_RX_QUEUE_SIZE + 3; ++i) {
		uint8_t tester_present[] = {0x3e, 0x80};

		inject(tester_present, sizeof(tester_present));
		/* Wait for dispatcher delivery while the worker is deliberately blocked. */
		can_if_stats_t can_stats;
		int64_t deadline = k_uptime_get() + 50;

		do {
			can_if_get_stats(&can_stats);
			if (stats().rx_queued + stats().rx_dropped >= queued + i + 1) { break; }
			zassert_true(k_uptime_get() < deadline);
			k_sleep(K_MSEC(1));
		} while (true);
		zassert_equal(can_stats.rx_dropped, 0);
	}
	zassert_equal(stats().rx_dropped, 3);
	k_sem_give(&handler_release);
	expect((uint8_t[]){0x7f, 0x22, 0x22}, 3);
}

static void immediate_response_latency(void)
{
	/* Consecutive requests expose upstream's rearmed response pacing timer. */
	for (size_t i = 0; i < 4; ++i) {
		int64_t request_time = k_uptime_get();

		inject((uint8_t[]){0x3e, 0}, 2);
		expect((uint8_t[]){0x7e, 0}, 2);
		zassert_true(k_uptime_get() - request_time <= 8,
			     "Immediate handler response delayed by internal pacing");
	}
}

ZTEST(diag_uds, test_contract)
{
	lifecycle_and_port();
	immediate_response_latency();
	inject((uint8_t[]){0x22, 0, 1}, 3);
	expect((uint8_t[]){0x62, 0, 1, 0x11, 0x22}, 5);
	inject((uint8_t[]){0x31, 1, 0, 1}, 4);
	expect((uint8_t[]){0x71, 1, 0, 1}, 4);
	inject((uint8_t[]){0x31, 3, 0, 1}, 4);
	expect((uint8_t[]){0x7f, 0x31, 0x10}, 3);
	zassert_equal(stats().handler_errors, 1);
	inject((uint8_t[]){0x34}, 1);
	expect((uint8_t[]){0x7f, 0x34, 0x11}, 3);
	oversized_result = true;
	inject((uint8_t[]){0x22, 0, 1}, 3);
	expect((uint8_t[]){0x7f, 0x22, 0x14}, 3);
	oversized_result = false;
	completion_failure = -EIO;
	inject((uint8_t[]){0x22, 0, 1}, 3);
	expect((uint8_t[]){0x62, 0, 1, 0x11, 0x22}, 5);
	zassert_equal(stats().tx_completion_errors, 2);
	completion_failure = 0;
	k_sleep(K_MSEC(2));
	inject((uint8_t[]){0x3e, 0}, 2);
	expect((uint8_t[]){0x7e, 0}, 2);
	queue_overflow();
	zassert_equal(atomic_get(&handler_context_errors), 0);
	zassert_equal(diag_uds_get_stats(NULL), -EINVAL);
}

ZTEST_SUITE(diag_uds, NULL, NULL, NULL, NULL, NULL);
