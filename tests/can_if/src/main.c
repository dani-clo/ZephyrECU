#include "can_if.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;

static const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static can_rx_callback_t ingress;
static can_state_change_callback_t state_callback;
static can_tx_callback_t completion;
static void *completion_context;
static const struct can_frame *retained_tx;
static struct can_frame injected;
static atomic_t received;
static atomic_t fanout;
static atomic_t extended_received;
static atomic_t tx_notifications;
static atomic_t callback_in_isr;
static bool block_rx;
static can_if_frame_t last_rx;
static int completion_status;
K_SEM_DEFINE(callback_entered, 0, 1);
K_SEM_DEFINE(callback_release, 0, 1);

static void rx(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(context);
	if (k_is_in_isr()) {
		atomic_set(&callback_in_isr, 1);
	}
	last_rx = *frame;
	if (block_rx) {
		block_rx = false;
		k_sem_give(&callback_entered);
		k_sem_take(&callback_release, K_FOREVER);
	}
	atomic_inc(&received);
}

static void rx_fanout(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(context);
	atomic_inc(&fanout);
}

static void rx_extended(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(context);
	zassert_true(frame->extended);
	atomic_inc(&extended_received);
}

static int add_filter(const struct device *device, can_rx_callback_t cb,
		      void *context, const struct can_filter *filter)
{
	ARG_UNUSED(device);
	ARG_UNUSED(context);
	ingress = cb;
	zassert_equal(filter->mask, (filter->flags & CAN_FILTER_IDE) ?
		      CAN_EXT_ID_MASK : CAN_STD_ID_MASK);
	return (int)fake_can_add_rx_filter_fake.call_count - 1;
}

static void inject_irq(const void *unused)
{
	ARG_UNUSED(unused);
	ingress(dev, &injected, NULL);
}

static void inject(uint32_t id, uint8_t flags, uint8_t dlc, uint8_t value)
{
	injected = (struct can_frame){.id = id, .flags = flags, .dlc = dlc};
	injected.data[0] = value;
	irq_offload(inject_irq, NULL);
	/* A driver-owned buffer may be reused immediately. */
	memset(&injected, 0xa5, sizeof(injected));
}

static can_if_stats_t stats(void)
{
	can_if_stats_t result;

	zassert_ok(can_if_get_stats(&result));
	return result;
}

static void wait_rx(uint32_t handled)
{
	int64_t deadline = k_uptime_get() + 1000;

	while (true) {
		can_if_stats_t s = stats();

		if (s.rx_dispatched + s.rx_invalid + s.rx_unmatched >= handled) {
			return;
		}
		zassert_true(k_uptime_get() < deadline, "dispatcher did not drain queue");
		k_sleep(K_MSEC(1));
	}
}

static void lifecycle_and_filters(void)
{
	can_if_frame_t frame = {.id = 1};

	zassert_equal(can_if_send(&frame, NULL, NULL), -ENETDOWN);
	zassert_equal(can_if_start(), -EACCES);
	zassert_equal(can_if_register_rx(0x100, false, rx, NULL), -EACCES);
	fake_can_set_mode_fake.return_val = -EIO;
	zassert_equal(can_if_init(), -EIO);
	fake_can_set_mode_fake.return_val = 0;
	zassert_ok(can_if_init());
	zassert_equal(can_if_init(), -EALREADY);
	zassert_equal(fake_can_set_mode_fake.arg1_val, CAN_MODE_NORMAL);
	state_callback = fake_can_set_state_change_callback_fake.arg1_val;
	zassert_not_null(state_callback);
	zassert_equal(can_if_register_rx(0x800, false, rx, NULL), -EINVAL);
	zassert_equal(can_if_register_rx(0x20000000, true, rx, NULL), -EINVAL);
	zassert_equal(can_if_register_rx(0x100, false, NULL, NULL), -EINVAL);
	fake_can_add_rx_filter_fake.return_val = -ENOSPC;
	zassert_equal(can_if_register_rx(0x100, false, rx, NULL), -ENOSPC);
	fake_can_add_rx_filter_fake.custom_fake = add_filter;
	zassert_ok(can_if_register_rx(0x100, false, rx, NULL));
	unsigned int filters = fake_can_add_rx_filter_fake.call_count;

	zassert_ok(can_if_register_rx(0x100, false, rx_fanout, NULL));
	zassert_equal(can_if_register_rx(0x100, false, rx, NULL), -EALREADY);
	zassert_equal(can_if_register_rx(0x100, false, rx, &received), -ENOSPC);
	zassert_equal(fake_can_add_rx_filter_fake.call_count, filters);
	zassert_ok(can_if_register_rx(0x100, true, rx_extended, NULL));
	zassert_ok(can_if_register_rx(0x101, false, rx, NULL));
	zassert_equal(can_if_register_rx(0x102, false, rx, NULL), -ENOSPC);
	fake_can_start_fake.return_val = -EIO;
	zassert_equal(can_if_start(), -EIO);
	fake_can_start_fake.return_val = 0;
	zassert_ok(can_if_start());
	zassert_equal(can_if_start(), -EALREADY);
	zassert_equal(fake_can_start_fake.call_count, 2); /* one failure, one success */
	zassert_equal(can_if_register_rx(0x100, false, rx, NULL), -EBUSY);
}

static void rx_validation_and_overflow(void)
{
	inject(0x100, 0, 1, 42);
	wait_rx(1);
	zassert_equal(atomic_get(&received), 1);
	zassert_equal(atomic_get(&fanout), 1);
	zassert_equal(last_rx.data[0], 42);
	zassert_equal(last_rx.data[1], 0);
	zassert_equal(atomic_get(&callback_in_isr), 0);
	const struct { uint32_t id; uint8_t flags; uint8_t dlc; } frames[] = {
		{0x100, CAN_FRAME_IDE, 1}, {0x102, 0, 1},
		{0x101, CAN_FRAME_IDE, 1}, {0x100, CAN_FRAME_RTR, 1},
		{0x100, CAN_FRAME_FDF, 1}, {0x100, 0, 9}, {0x800, 0, 1},
	};

	for (size_t i = 0; i < ARRAY_SIZE(frames); ++i) {
		inject(frames[i].id, frames[i].flags, frames[i].dlc, 12);
		wait_rx((uint32_t)i + 2);
	}
	zassert_equal(atomic_get(&extended_received), 1);
	zassert_equal(stats().rx_invalid, 4);
	zassert_equal(stats().rx_unmatched, 2);
	/* Classical DLC boundaries and complete eight-byte ownership. */
	inject(0x101, 0, 0, 99);
	wait_rx(stats().rx_queued);
	zassert_equal(last_rx.dlc, 0);
	zassert_equal(last_rx.data[0], 0);
	injected = (struct can_frame){.id = 0x101, .dlc = 8,
		.data = {1, 2, 3, 4, 5, 6, 7, 8}};
	irq_offload(inject_irq, NULL);
	memset(&injected, 0xa5, sizeof(injected));
	wait_rx(stats().rx_queued);
	zassert_equal(last_rx.dlc, 8);
	zassert_equal(last_rx.data[0], 1);
	zassert_equal(last_rx.data[7], 8);
	/* Overflow is tested separately, without timing dependence. */
	can_if_stats_t before = stats();

	block_rx = true;
	inject(0x101, 0, 1, 77);
	zassert_ok(k_sem_take(&callback_entered, K_SECONDS(1)));
	for (int i = 0; i < CONFIG_CAN_IF_RX_QUEUE_SIZE + 3; ++i) {
		inject(0x101, 0, 1, 88);
	}
	zassert_equal(stats().rx_dropped - before.rx_dropped, 3);
	k_sem_give(&callback_release);
	wait_rx(stats().rx_queued);
	zassert_equal(last_rx.data[0], 88);
	zassert_equal(stats().rx_dispatched - before.rx_dispatched,
		      CONFIG_CAN_IF_RX_QUEUE_SIZE + 1);
}

static int send_capture(const struct device *device, const struct can_frame *frame,
			k_timeout_t timeout, can_tx_callback_t cb, void *context)
{
	ARG_UNUSED(device);
	zassert_true(K_TIMEOUT_EQ(timeout, K_NO_WAIT));
	retained_tx = frame;
	completion = cb;
	completion_context = context;
	return 0;
}

static void notified(int status, void *context)
{
	zassert_equal(context, &received);
	completion_status = status;
	atomic_inc(&tx_notifications);
}

static void complete_irq(const void *status)
{
	completion(dev, *(const int *)status, completion_context);
}

static int send_inline(const struct device *device, const struct can_frame *frame,
		       k_timeout_t timeout, can_tx_callback_t cb, void *context)
{
	int ret = send_capture(device, frame, timeout, cb, context);

	cb(device, 0, context);
	return ret;
}

static void tx_failures_and_ownership(void)
{
	can_if_frame_t frame = {.id = 0x123, .dlc = 1, .data = {0x55}};

	zassert_equal(can_if_send(NULL, NULL, NULL), -EINVAL);
	frame.dlc = 9;
	zassert_equal(can_if_send(&frame, NULL, NULL), -EINVAL);
	frame.dlc = 1;
	frame.id = 0x800;
	zassert_equal(can_if_send(&frame, NULL, NULL), -EINVAL);
	frame.id = 0x20000000;
	frame.extended = true;
	zassert_equal(can_if_send(&frame, NULL, NULL), -EINVAL);
	frame.id = 0x123;
	frame.extended = false;
	fake_can_send_fake.return_val = -EIO;
	zassert_equal(can_if_send(&frame, notified, &received), -EIO);
	zassert_equal(atomic_get(&tx_notifications), 0);
	zassert_false(stats().tx_pending);
	zassert_equal(stats().tx_submit_errors, 1);
	fake_can_send_fake.custom_fake = send_capture;
	zassert_ok(can_if_send(&frame, notified, &received));
	frame.data[0] = 0x99;
	zassert_equal(retained_tx->data[0], 0x55);
	zassert_equal(can_if_send(&frame, NULL, NULL), -EAGAIN);
	zassert_true(stats().tx_pending);
	int status = -ENETUNREACH;

	irq_offload(complete_irq, &status);
	zassert_equal(completion_status, status);
	zassert_equal(stats().tx_completion_errors, 1);
	zassert_equal(stats().last_tx_error, status);
	zassert_false(stats().tx_pending);
	zassert_ok(can_if_send(&frame, NULL, NULL));
	status = 0;
	irq_offload(complete_irq, &status);
	fake_can_send_fake.custom_fake = send_inline;
	zassert_ok(can_if_send(&frame, notified, &received));
	zassert_false(stats().tx_pending);
	zassert_equal(stats().tx_submitted, 3);
	zassert_equal(stats().tx_completed, 2);
	zassert_equal(stats().tx_busy, 1);
	frame.id = CAN_EXT_ID_MASK;
	frame.extended = true;
	frame.dlc = 8;
	frame.data[7] = 0xAB;
	zassert_ok(can_if_send(&frame, NULL, NULL));
	zassert_equal(retained_tx->id, CAN_EXT_ID_MASK);
	zassert_equal(retained_tx->flags, CAN_FRAME_IDE);
	zassert_equal(retained_tx->data[7], 0xAB);
	frame.extended = false;
	frame.id = CAN_STD_ID_MASK;
	frame.dlc = 0;
	zassert_ok(can_if_send(&frame, NULL, NULL));
	zassert_equal(retained_tx->dlc, 0);
	zassert_equal(retained_tx->data[0], 0);
	zassert_equal(stats().tx_submitted, 5);
	zassert_equal(stats().tx_completed, 4);
}

static void state_irq(const void *unused)
{
	ARG_UNUSED(unused);
	state_callback(dev, CAN_STATE_BUS_OFF,
		       (struct can_bus_err_cnt){.tx_err_cnt = 255, .rx_err_cnt = 17}, NULL);
}


static void public_api_irq(const void *unused)
{
	ARG_UNUSED(unused);
	can_if_frame_t frame = {.id = 1};

	zassert_equal(can_if_init(), -EWOULDBLOCK);
	zassert_equal(can_if_start(), -EWOULDBLOCK);
	zassert_equal(can_if_register_rx(1, false, rx, NULL), -EWOULDBLOCK);
	zassert_equal(can_if_send(&frame, NULL, NULL), -EWOULDBLOCK);
}

ZTEST(can_if, test_contract)
{
	irq_offload(public_api_irq, NULL);
	lifecycle_and_filters();
	rx_validation_and_overflow();
	tx_failures_and_ownership();
	irq_offload(state_irq, NULL);
	zassert_equal(stats().state, CAN_IF_BUS_OFF);
	zassert_equal(stats().bus_off, 1);
	zassert_equal(stats().tx_error_count, 255);
	zassert_equal(stats().rx_error_count, 17);
	zassert_equal(can_if_get_stats(NULL), -EINVAL);
}

ZTEST_SUITE(can_if, NULL, NULL, NULL, NULL, NULL);
