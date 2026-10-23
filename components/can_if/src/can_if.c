#include "can_if.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(can_if);

struct consumer {
	can_if_rx_callback_t callback;
	void *context;
};

struct route {
	uint32_t id;
	bool extended;
	size_t count;
	struct consumer consumers[CONFIG_CAN_IF_MAX_CONSUMERS];
};

static const struct device *const controller = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static struct route routes[CONFIG_CAN_IF_MAX_ROUTES];
static size_t route_count;
static bool initialized;
static atomic_t started;
K_MUTEX_DEFINE(lifecycle_lock);
K_MUTEX_DEFINE(submit_lock);
K_MSGQ_DEFINE(rx_queue, sizeof(struct can_frame), CONFIG_CAN_IF_RX_QUEUE_SIZE, 4);

#define COUNTERS(X) \
	X(rx_queued) X(rx_dispatched) X(rx_callbacks) X(rx_dropped) \
	X(rx_invalid) X(rx_unmatched) X(tx_submitted) X(tx_completed) \
	X(tx_submit_errors) X(tx_completion_errors) X(tx_busy) X(state_changes) X(bus_off)
#define DECLARE_COUNTER(name) static atomic_t name;
COUNTERS(DECLARE_COUNTER)
static atomic_t tx_pending;
static atomic_t last_tx_error;
static atomic_t last_state = ATOMIC_INIT(CAN_IF_STOPPED);
static atomic_t tx_error_count;
static atomic_t rx_error_count;
/* Retained until completion, including when the caller's buffer goes away. */
static struct can_frame tx_frame;
static can_if_tx_callback_t tx_callback;
static void *tx_context;

static bool valid_id(uint32_t id, bool extended)
{
	return id <= (extended ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK);
}

static can_if_state_t convert_state(enum can_state state)
{
	switch (state) {
	case CAN_STATE_STOPPED: return CAN_IF_STOPPED;
	case CAN_STATE_ERROR_ACTIVE: return CAN_IF_ERROR_ACTIVE;
	case CAN_STATE_ERROR_WARNING: return CAN_IF_ERROR_WARNING;
	case CAN_STATE_ERROR_PASSIVE: return CAN_IF_ERROR_PASSIVE;
	case CAN_STATE_BUS_OFF: return CAN_IF_BUS_OFF;
	default: return CAN_IF_STATE_UNKNOWN;
	}
}

static void state_changed(const struct device *dev, enum can_state state,
			  struct can_bus_err_cnt errors, void *context)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(context);
	atomic_set(&last_state, convert_state(state));
	atomic_set(&tx_error_count, errors.tx_err_cnt);
	atomic_set(&rx_error_count, errors.rx_err_cnt);
	atomic_inc(&state_changes);
	if (state == CAN_STATE_BUS_OFF) {
		atomic_inc(&bus_off);
	}
}

static void rx_ingress(const struct device *dev, struct can_frame *frame, void *context)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(context);
	/* ISR safe: queue copies the entire frame. No decoding, logging or clients. */
	if (k_msgq_put(&rx_queue, frame, K_NO_WAIT) != 0) {
		atomic_inc(&rx_dropped);
	} else {
		atomic_inc(&rx_queued);
	}
}

static void dispatcher(void *a, void *b, void *c)
{
	struct can_frame raw;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		k_msgq_get(&rx_queue, &raw, K_FOREVER);
		bool extended = (raw.flags & CAN_FRAME_IDE) != 0;

		if ((raw.flags & ~CAN_FRAME_IDE) != 0 || raw.dlc > 8 ||
		    !valid_id(raw.id, extended)) {
			atomic_inc(&rx_invalid);
			continue;
		}
		can_if_frame_t frame = {.id = raw.id, .dlc = raw.dlc, .extended = extended};

		memcpy(frame.data, raw.data, raw.dlc);
		bool matched = false;

		for (size_t i = 0; i < route_count; ++i) {
			const struct route *route = &routes[i];

			if (route->id != frame.id || route->extended != frame.extended) {
				continue;
			}
			matched = true;
			for (size_t j = 0; j < route->count; ++j) {
				route->consumers[j].callback(&frame, route->consumers[j].context);
				atomic_inc(&rx_callbacks);
			}
			break;
		}
		atomic_inc(matched ? &rx_dispatched : &rx_unmatched);
	}
}

K_THREAD_DEFINE(dispatcher_id, CONFIG_CAN_IF_DISPATCH_STACK_SIZE, dispatcher,
		NULL, NULL, NULL, CONFIG_CAN_IF_DISPATCH_PRIORITY, 0, SYS_FOREVER_MS);

int can_if_init(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_mutex_lock(&lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret = 0;

	if (initialized) {
		ret = -EALREADY;
	} else if (!device_is_ready(controller)) {
		ret = -ENODEV;
	} else {
		/* Explicit normal mode excludes FD and loopback. Bitrate stays in DT. */
		ret = can_set_mode(controller, CAN_MODE_NORMAL);
		if (ret == 0) {
			can_set_state_change_callback(controller, state_changed, NULL);
			initialized = true;
		}
	}
	k_mutex_unlock(&lifecycle_lock);
	return ret;
}

int can_if_register_rx(uint32_t id, bool extended,
		       can_if_rx_callback_t callback, void *context)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (callback == NULL || !valid_id(id, extended)) {
		return -EINVAL;
	}
	if (k_mutex_lock(&lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret = 0;
	struct route *route = NULL;

	if (!initialized || atomic_get(&started)) {
		ret = initialized ? -EBUSY : -EACCES;
		goto out;
	}
	for (size_t i = 0; i < route_count; ++i) {
		if (routes[i].id == id && routes[i].extended == extended) {
			route = &routes[i];
			break;
		}
	}
	if (route == NULL) {
		if (route_count == ARRAY_SIZE(routes)) {
			ret = -ENOSPC;
			goto out;
		}
		struct can_filter filter = {
			.id = id,
			.mask = extended ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK,
			.flags = extended ? CAN_FILTER_IDE : 0,
		};

		ret = can_add_rx_filter(controller, rx_ingress, NULL, &filter);
		if (ret < 0) {
			goto out;
		}
		route = &routes[route_count++];
		route->id = id;
		route->extended = extended;
	}
	for (size_t j = 0; j < route->count; ++j) {
		if (route->consumers[j].callback == callback &&
		    route->consumers[j].context == context) {
			ret = -EALREADY;
			goto out;
		}
	}
	if (route->count == ARRAY_SIZE(route->consumers)) {
		ret = -ENOSPC;
		goto out;
	}
	route->consumers[route->count++] = (struct consumer){callback, context};
	ret = 0;
out:
	k_mutex_unlock(&lifecycle_lock);
	return ret;
}

int can_if_start(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_mutex_lock(&lifecycle_lock, K_NO_WAIT) != 0) {
		return -EBUSY;
	}
	int ret;

	if (!initialized || atomic_get(&started)) {
		ret = initialized ? -EALREADY : -EACCES;
	} else {
		ret = can_start(controller);
		if (ret == 0) {
			enum can_state state = CAN_STATE_ERROR_ACTIVE;
			struct can_bus_err_cnt errors = {0};

			if (can_get_state(controller, &state, &errors) == 0) {
				atomic_set(&last_state, convert_state(state));
				atomic_set(&tx_error_count, errors.tx_err_cnt);
				atomic_set(&rx_error_count, errors.rx_err_cnt);
			}
			atomic_set(&started, 1);
			k_thread_start(dispatcher_id);
			LOG_INF("Started %s; %u RX routes", controller->name,
				(unsigned int)route_count);
		}
	}
	k_mutex_unlock(&lifecycle_lock);
	return ret;
}

static void tx_done(const struct device *dev, int status, void *context)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(context);
	if (status != 0) {
		atomic_set(&last_tx_error, status);
		atomic_inc(&tx_completion_errors);
	} else {
		atomic_inc(&tx_completed);
	}
	/* Keep slot occupied until client callback returns; it cannot send again. */
	if (tx_callback != NULL) {
		tx_callback(status, tx_context);
	}
	atomic_clear(&tx_pending);
}

int can_if_send(const can_if_frame_t *frame, can_if_tx_callback_t callback, void *context)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (frame == NULL || frame->dlc > 8 || !valid_id(frame->id, frame->extended)) {
		return -EINVAL;
	}
	if (!atomic_get(&started)) {
		return -ENETDOWN;
	}
	if (k_mutex_lock(&submit_lock, K_NO_WAIT) != 0) {
		atomic_inc(&tx_busy);
		return -EAGAIN;
	}
	int ret;

	if (!atomic_cas(&tx_pending, 0, 1)) {
		atomic_inc(&tx_busy);
		ret = -EAGAIN;
		goto out;
	}
	tx_frame = (struct can_frame){
		.id = frame->id,
		.dlc = frame->dlc,
		.flags = frame->extended ? CAN_FRAME_IDE : 0,
	};
	memcpy(tx_frame.data, frame->data, frame->dlc);
	tx_callback = callback;
	tx_context = context;
	ret = can_send(controller, &tx_frame, K_NO_WAIT, tx_done, NULL);
	if (ret != 0) {
		atomic_set(&last_tx_error, ret);
		atomic_inc(&tx_submit_errors);
		atomic_clear(&tx_pending);
	} else {
		atomic_inc(&tx_submitted);
	}
out:
	k_mutex_unlock(&submit_lock);
	return ret;
}

int can_if_get_stats(can_if_stats_t *stats)
{
	if (stats == NULL) {
		return -EINVAL;
	}
#define READ_COUNTER(name) stats->name = (uint32_t)atomic_get(&name);
	COUNTERS(READ_COUNTER)
	stats->tx_pending = atomic_get(&tx_pending) != 0;
	stats->last_tx_error = (int)atomic_get(&last_tx_error);
	stats->state = (can_if_state_t)atomic_get(&last_state);
	stats->tx_error_count = (uint8_t)atomic_get(&tx_error_count);
	stats->rx_error_count = (uint8_t)atomic_get(&rx_error_count);
	return 0;
}
