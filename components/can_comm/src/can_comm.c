#include "can_comm_private.h"
#include "can_if.h"
#include "can_signals.h"

#include <errno.h>
#include <string.h>

struct can_comm_pdu_state can_comm_pdus[CAN_PDU_COUNT];
can_comm_stats_t can_comm_stats;
K_MUTEX_DEFINE(can_comm_lock);
static int64_t raw_cache[CAN_SIGNAL_COUNT];
static bool initialized, started;
static unsigned int registered_routes;
static struct {
	can_comm_pdu_rx_cb_t cb;
	void *context;
} pdu_callbacks[CAN_PDU_COUNT][CONFIG_CAN_COMM_MAX_CALLBACKS];
static struct {
	can_comm_signal_cb_t cb;
	void *context;
} signal_callbacks[CAN_SIGNAL_COUNT][CONFIG_CAN_COMM_MAX_CALLBACKS];

static bool pdu_ok(can_pdu_id_t pdu)
{
	return (unsigned int)pdu < CAN_PDU_COUNT;
}

static bool signal_ok(can_signal_id_t signal)
{
	return (unsigned int)signal < CAN_SIGNAL_COUNT;
}

static can_comm_sample_t sample_locked(can_signal_id_t signal, int64_t now)
{
	const struct can_signal_cfg *cfg = &can_signal_config[signal];
	const struct can_pdu_cfg *pdu = &can_pdu_config[cfg->pdu];
	const struct can_comm_pdu_state *state = &can_comm_pdus[cfg->pdu];
	bool tx = pdu->mode != CAN_COMM_RX;
	int64_t age = state->received ? now - state->received_at : 0;

	return (can_comm_sample_t){
		.raw = raw_cache[signal],
		.physical = (double)raw_cache[signal] * cfg->scale + cfg->offset,
		.age_ms = age, .received = state->received,
		.valid = tx || (state->received && age < pdu->timeout_ms),
	};
}

static void receive(const can_if_frame_t *frame, void *context)
{
	const struct can_pdu_cfg *cfg = context;
	can_pdu_id_t pdu = (can_pdu_id_t)(cfg - can_pdu_config);
	can_comm_pdu_sample_t snapshot = {.pdu = pdu, .timestamp_ms = k_uptime_get()};
	bool changed[CAN_COMM_MAX_PDU_SIGNALS] = {false};
	can_comm_sample_t samples[CAN_COMM_MAX_PDU_SIGNALS];

	k_mutex_lock(&can_comm_lock, K_FOREVER);
	if (!started) {
		k_mutex_unlock(&can_comm_lock);
		return;
	}
	bool valid = frame->dlc == cfg->dlc && frame->id == cfg->id &&
		frame->extended == cfg->extended;

	for (unsigned int i = 0; valid && i < cfg->signal_count; ++i) {
		can_signal_id_t signal = (can_signal_id_t)(cfg->first_signal + i);
		const struct can_signal_cfg *sig = &can_signal_config[signal];

		snapshot.raw[i] = can_signal_decode(signal, frame->data);
		valid = snapshot.raw[i] >= sig->raw_min && snapshot.raw[i] <= sig->raw_max;
	}
	if (!valid) {
		++can_comm_stats.rx_invalid;
		k_mutex_unlock(&can_comm_lock);
		return;
	}
	struct can_comm_pdu_state *state = &can_comm_pdus[pdu];

	if (state->valid && snapshot.timestamp_ms - state->received_at >= cfg->timeout_ms) {
		++can_comm_stats.rx_timeouts;
	}
	memcpy(snapshot.data, frame->data, cfg->dlc);
	for (unsigned int i = 0; i < cfg->signal_count; ++i) {
		unsigned int signal = cfg->first_signal + i;

		changed[i] = !state->received || raw_cache[signal] != snapshot.raw[i];
		raw_cache[signal] = snapshot.raw[i];
	}
	state->received = true;
	state->valid = true;
	state->received_at = snapshot.timestamp_ms;
	memcpy(state->data, snapshot.data, sizeof(state->data));
	++can_comm_stats.rx_pdus;
	for (unsigned int i = 0; i < cfg->signal_count; ++i) {
		samples[i] = sample_locked((can_signal_id_t)(cfg->first_signal + i), snapshot.timestamp_ms);
	}
	k_mutex_unlock(&can_comm_lock);
	/* Registration is immutable while running; no client code holds cache lock. */
	for (unsigned int j = 0; j < CONFIG_CAN_COMM_MAX_CALLBACKS; ++j) {
		if (pdu_callbacks[pdu][j].cb != NULL) {
			pdu_callbacks[pdu][j].cb(&snapshot, pdu_callbacks[pdu][j].context);
		}
	}
	for (unsigned int i = 0; i < cfg->signal_count; ++i) {
		can_signal_id_t signal = (can_signal_id_t)(cfg->first_signal + i);

		for (unsigned int j = 0; changed[i] && j < CONFIG_CAN_COMM_MAX_CALLBACKS; ++j) {
			if (signal_callbacks[signal][j].cb != NULL) {
				signal_callbacks[signal][j].cb(signal, &samples[i], signal_callbacks[signal][j].context);
			}
		}
	}
}

int can_comm_init(void)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = initialized ? -EALREADY : 0;

	while (!initialized && registered_routes < CAN_PDU_COUNT) {
		const struct can_pdu_cfg *cfg = &can_pdu_config[registered_routes];

		if (cfg->mode == CAN_COMM_RX) {
			ret = can_if_register_rx(cfg->id, cfg->extended, receive, (void *)cfg);
			if (ret != 0) { break; }
		}
		++registered_routes;
	}
	if (!initialized && ret == 0) {
		for (unsigned int i = 0; i < CAN_SIGNAL_COUNT; ++i) {
			raw_cache[i] = can_signal_config[i].initial;
			can_signal_encode((can_signal_id_t)i, raw_cache[i],
					  can_comm_pdus[can_signal_config[i].pdu].data);
		}
		initialized = true;
	}
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_start(void)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = !initialized ? -EACCES : (started ? -EALREADY : 0);
	can_if_stats_t bus;

	can_if_get_stats(&bus);
	if (ret == 0 && bus.state == CAN_IF_STOPPED) { ret = -EACCES; }
	if (ret == 0) {
		started = true;
		can_comm_scheduler_start(k_uptime_get());
	}
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_register_pdu_rx_callback(can_pdu_id_t pdu, can_comm_pdu_rx_cb_t cb, void *context)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!pdu_ok(pdu) || cb == NULL) { return -EINVAL; }
	if (can_pdu_config[pdu].mode != CAN_COMM_RX) { return -EACCES; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = !initialized ? -EACCES : (started ? -EBUSY : -ENOSPC);

	if (initialized && !started) {
		for (unsigned int i = 0; i < CONFIG_CAN_COMM_MAX_CALLBACKS; ++i) {
			if (pdu_callbacks[pdu][i].cb == cb && pdu_callbacks[pdu][i].context == context) {
				ret = -EALREADY;
				break;
			}
			if (pdu_callbacks[pdu][i].cb == NULL) {
				pdu_callbacks[pdu][i].cb = cb;
				pdu_callbacks[pdu][i].context = context;
				ret = 0;
				break;
			}
		}
	}
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_register_signal_callback(can_signal_id_t signal, can_comm_signal_cb_t cb, void *context)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!signal_ok(signal) || cb == NULL) { return -EINVAL; }
	if (can_pdu_config[can_signal_config[signal].pdu].mode != CAN_COMM_RX) { return -EACCES; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = !initialized ? -EACCES : (started ? -EBUSY : -ENOSPC);

	if (initialized && !started) {
		for (unsigned int i = 0; i < CONFIG_CAN_COMM_MAX_CALLBACKS; ++i) {
			if (signal_callbacks[signal][i].cb == cb && signal_callbacks[signal][i].context == context) {
				ret = -EALREADY;
				break;
			}
			if (signal_callbacks[signal][i].cb == NULL) {
				signal_callbacks[signal][i].cb = cb;
				signal_callbacks[signal][i].context = context;
				ret = 0;
				break;
			}
		}
	}
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_get_signal(can_signal_id_t signal, can_comm_sample_t *sample)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!signal_ok(signal) || sample == NULL) { return -EINVAL; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = initialized ? 0 : -EACCES;

	if (ret == 0) { *sample = sample_locked(signal, k_uptime_get()); }
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_set_signal_raw(can_signal_id_t signal, int64_t raw)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!signal_ok(signal)) { return -EINVAL; }
	const struct can_signal_cfg *cfg = &can_signal_config[signal];

	if (can_pdu_config[cfg->pdu].mode == CAN_COMM_RX) { return -EACCES; }
	if (raw < cfg->raw_min || raw > cfg->raw_max) { return -ERANGE; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = initialized ? 0 : -EACCES;

	if (ret == 0 && raw_cache[signal] != raw) {
		raw_cache[signal] = raw;
		can_signal_encode(signal, raw, can_comm_pdus[cfg->pdu].data);
		if (cfg->on_change && can_pdu_config[cfg->pdu].mode != CAN_COMM_PERIODIC) {
			can_comm_pdus[cfg->pdu].pending_trigger = true;
		}
	}
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_trigger_tx(can_pdu_id_t pdu)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (!pdu_ok(pdu)) { return -EINVAL; }
	if (can_pdu_config[pdu].mode != CAN_COMM_EVENT &&
	    can_pdu_config[pdu].mode != CAN_COMM_MIXED) { return -EACCES; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	int ret = initialized ? 0 : -EACCES;

	if (ret == 0) { can_comm_pdus[pdu].pending_trigger = true; }
	k_mutex_unlock(&can_comm_lock);
	return ret;
}

int can_comm_get_stats(can_comm_stats_t *stats)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	if (stats == NULL) { return -EINVAL; }
	k_mutex_lock(&can_comm_lock, K_FOREVER);
	*stats = can_comm_stats;
	k_mutex_unlock(&can_comm_lock);
	return 0;
}
