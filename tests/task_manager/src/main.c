#include "ecu_task_manager.h"
#include "task_manager_private.h"
#include "app_led.h"
#include "app_tasks.h"
#include "can_if.h"
#include "can_comm.h"
#include "can_signals.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;
extern const k_tid_t ecu_task_thread;
static atomic_t in_callback, tx_count;
static can_rx_callback_t ingress;
static void *ingress_context;
static const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
struct record { uint32_t events; int task; int64_t at; };
K_MSGQ_DEFINE(records, sizeof(struct record), 128, 4);
ECU_TASK_DECLARE(test_c_event);
ECU_TASK_DECLARE(app_led_task);

static void record(int task, uint32_t events)
{
	zassert_false(k_is_in_isr());
	zassert_equal(k_current_get(), ecu_task_thread);
	zassert_true(atomic_cas(&in_callback, 0, 1));
	if (task == 2 && events == 4) {
		/* Simulate a producer arriving after pending bits were consumed. */
		zassert_ok(ecu_task_notify(test_c_event, 8));
	}
	struct record value = {.task = task, .events = events, .at = k_uptime_get()};

	zassert_ok(k_msgq_put(&records, &value, K_NO_WAIT));
	atomic_clear(&in_callback);
}
static void fast(uint32_t events) { record(0, events); }
static void slow(uint32_t events) { record(1, events); }
static void event(uint32_t events) { record(2, events); }
ECU_TASK_DEFINE(test_a_fast, fast, 10);
ECU_TASK_DEFINE(test_b_slow, slow, 30);
ECU_TASK_DEFINE(test_c_event, event, 0);

static int add_filter(const struct device *device, can_rx_callback_t cb,
		      void *context, const struct can_filter *filter)
{
	ARG_UNUSED(device);
	if (filter->id == can_pdu_config[CAN_PDU_LED_COMMAND].id) {
		ingress = cb;
		ingress_context = context;
	}
	return (int)fake_can_add_rx_filter_fake.call_count;
}
static int send_frame(const struct device *device, const struct can_frame *frame,
		      k_timeout_t timeout, can_tx_callback_t cb, void *context)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(timeout);
	atomic_inc(&tx_count);
	cb(device, 0, context);
	return 0;
}
static void notify_irq(const void *unused)
{
	ARG_UNUSED(unused);
	zassert_true(k_is_in_isr());
	zassert_equal(ecu_task_manager_start(), -EWOULDBLOCK);
	zassert_ok(ecu_task_notify(test_c_event, 0x80000000U));
}
static void inject_irq(const void *unused)
{
	ARG_UNUSED(unused);
	struct can_frame frame = {
		.id = can_pdu_config[CAN_PDU_LED_COMMAND].id,
		.dlc = can_pdu_config[CAN_PDU_LED_COMMAND].dlc,
	};

	can_signal_encode(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, 1, frame.data);
	ingress(dev, &frame, ingress_context);
	memset(frame.data, 0, sizeof(frame.data));
}

static void reset(ecu_task_id_t task, int64_t next)
{
	memset(task->state, 0, sizeof(*task->state));
	task->state->next = k_ms_to_ticks_ceil64(next);
}
static bool prepare(ecu_task_id_t task, int64_t ms, uint32_t *events)
{
	return ecu_task_prepare(task, k_ms_to_ticks_ceil64(ms), events);
}
static void complete(ecu_task_id_t task, int64_t ms)
{
	ecu_task_complete(task, k_ms_to_ticks_ceil64(ms));
}
static ecu_task_stats_t stats(ecu_task_id_t task)
{
	ecu_task_stats_t result;

	zassert_ok(ecu_task_get_stats(task, &result));
	return result;
}

ZTEST(task_manager, test_contract)
{
	struct ecu_task_descriptor foreign = {0};
	ecu_task_stats_t snapshot;

	zassert_equal(ecu_task_notify(NULL, 1), -EINVAL);
	zassert_equal(ecu_task_notify(&foreign, 1), -EINVAL);
	zassert_equal(ecu_task_notify(test_a_fast, 0), -EINVAL);
	zassert_equal(ecu_task_get_stats(&foreign, &snapshot), -EINVAL);
	zassert_equal(ecu_task_get_stats(test_a_fast, NULL), -EINVAL);
	zassert_ok(ecu_task_notify(test_c_event, 1));
	zassert_ok(ecu_task_notify(test_c_event, 1));
	irq_offload(notify_irq, NULL);
	k_sleep(K_MSEC(40));
	zassert_equal(k_msgq_num_used_get(&records), 0);
	zassert_equal(stats(test_c_event).invocations, 0);

	zassert_ok(app_led_init());
	zassert_ok(can_if_init());
	fake_can_add_rx_filter_fake.custom_fake = add_filter;
	fake_can_send_fake.custom_fake = send_frame;
	zassert_ok(can_comm_init());
	zassert_ok(app_tasks_init());
	zassert_ok(can_if_start());
	zassert_ok(can_comm_start());
	zassert_ok(ecu_task_manager_start());
	zassert_equal(ecu_task_manager_start(), -EALREADY);

	struct record r;
	int fast_count = 0, slow_count = 0;
	int64_t until = k_uptime_get() + 1000;
	bool prestart_seen = false;

	while (k_uptime_get() < until && (fast_count < 8 || slow_count < 3 || !prestart_seen)) {
		zassert_ok(k_msgq_get(&records, &r, K_MSEC(200)));
		if (r.task == 2) {
			zassert_equal(r.events, 0x80000001U);
			zassert_false(prestart_seen); /* repeated same bit coalesces */
			prestart_seen = true;
		} else {
			zassert_equal(r.events, 0); /* periodic-only */
			fast_count += r.task == 0;
			slow_count += r.task == 1;
		}
	}
	zassert_true(prestart_seen && fast_count >= 8 && slow_count >= 3);
	zassert_true(fast_count > slow_count);
	zassert_ok(ecu_task_notify(test_c_event, 4));
	unsigned int seen = 0;

	while (seen < 2) {
		zassert_ok(k_msgq_get(&records, &r, K_MSEC(200)));
		if (r.task == 2) { zassert_equal(r.events, seen++ == 0 ? 4 : 8); }
	}

	/* Real IRQ ingress -> CAN dispatcher -> application notification -> LED.
	 * can_comm TX runs concurrently on its own thread throughout this test.
	 */
	zassert_not_null(ingress);
	k_thread_suspend(ecu_task_thread);
	int previous_tx = atomic_get(&tx_count);

	k_sleep(K_MSEC(50));
	zassert_true(atomic_get(&tx_count) > previous_tx);
	irq_offload(inject_irq, NULL);
	until = k_uptime_get() + 30;
	while (atomic_get(&app_led_task->state->pending) == 0 && k_uptime_get() < until) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(atomic_get(&app_led_task->state->pending), BIT(0));
	zassert_false(app_led_get()); /* dispatcher only posted; no inline processing */
	k_thread_resume(ecu_task_thread);
	until = k_uptime_get() + 50;
	while (!app_led_get() && k_uptime_get() < until) { k_sleep(K_MSEC(1)); }
	zassert_true(app_led_get());
	/* Periodic validity check eventually returns an expired request to off. */
	until = k_uptime_get() + 250;
	while (app_led_get() && k_uptime_get() < until) { k_sleep(K_MSEC(1)); }
	zassert_false(app_led_get());

	/* Freeze only the manager for deterministic production-state transitions.
	 * No alternate clock, scheduler, or lifecycle API is added to production.
	 */
	k_thread_suspend(ecu_task_thread);
	uint32_t events;

	reset(test_a_fast, 10);
	zassert_false(prepare(test_a_fast, 9, &events));
	zassert_true(prepare(test_a_fast, 10, &events));
	zassert_equal(events, 0);
	complete(test_a_fast, 27);
	zassert_equal(test_a_fast->state->next, k_ms_to_ticks_ceil64(30));
	zassert_equal(stats(test_a_fast).missed_periods, 1); /* skipped 20 */
	zassert_false(prepare(test_a_fast, 27, &events));
	zassert_true(prepare(test_a_fast, 30, &events));
	complete(test_a_fast, 30);
	zassert_equal(stats(test_a_fast).late_invocations, 0);

	reset(test_a_fast, 10);
	zassert_true(prepare(test_a_fast, 35, &events)); /* represents release 30 */
	zassert_equal(stats(test_a_fast).missed_periods, 2);
	zassert_equal(stats(test_a_fast).late_invocations, 1);
	complete(test_a_fast, 40); /* completion equality skips release 40 */
	zassert_equal(stats(test_a_fast).missed_periods, 3);
	zassert_equal(test_a_fast->state->next, k_ms_to_ticks_ceil64(50));
	zassert_false(prepare(test_a_fast, 40, &events));

	reset(test_a_fast, 10);
	zassert_ok(ecu_task_notify(test_a_fast, 1));
	zassert_ok(ecu_task_notify(test_a_fast, 2));
	zassert_true(prepare(test_a_fast, 10, &events));
	zassert_equal(events, 3); /* coincident periodic/event: one invocation */
	zassert_ok(ecu_task_notify(test_a_fast, 8)); /* post during callback */
	complete(test_a_fast, 11);
	zassert_true(prepare(test_a_fast, 11, &events));
	zassert_equal(events, 8);
	complete(test_a_fast, 11);
	zassert_false(prepare(test_a_fast, 11, &events));
	zassert_equal(stats(test_a_fast).invocations, 2);

	reset(test_a_fast, 10);
	zassert_ok(ecu_task_notify(test_a_fast, 1));
	zassert_true(prepare(test_a_fast, 5, &events));
	complete(test_a_fast, 105); /* slow event callback skips 10..100 */
	zassert_equal(stats(test_a_fast).missed_periods, 10);
	zassert_equal(test_a_fast->state->next, k_ms_to_ticks_ceil64(110));
	zassert_false(prepare(test_a_fast, 105, &events));

	/* Simultaneous deadlines use Zephyr's lexical registration order. */
	reset(test_a_fast, 30);
	reset(test_b_slow, 30);
	unsigned int order = 0;
	STRUCT_SECTION_FOREACH(ecu_task_descriptor, task) {
		if (task != test_a_fast && task != test_b_slow) { continue; }
		zassert_equal(task, order++ == 0 ? test_a_fast : test_b_slow);
		zassert_true(prepare(task, 30, &events));
		complete(task, 30);
	}
	zassert_equal(order, 2);

	reset(test_c_event, 0);
	zassert_false(prepare(test_c_event, 100000, &events));
	zassert_ok(ecu_task_notify(test_c_event, UINT32_MAX));
	zassert_true(prepare(test_c_event, 100000, &events));
	zassert_equal(events, UINT32_MAX);
	complete(test_c_event, 100001);
	zassert_equal(stats(test_c_event).missed_periods, 0);

	reset(test_a_fast, 10);
	test_a_fast->state->stats = (ecu_task_stats_t){UINT64_MAX, UINT64_MAX, UINT64_MAX};
	zassert_true(prepare(test_a_fast, 35, &events));
	complete(test_a_fast, 100);
	snapshot = stats(test_a_fast);
	zassert_equal(snapshot.invocations, UINT64_MAX);
	zassert_equal(snapshot.missed_periods, UINT64_MAX);
	zassert_equal(snapshot.late_invocations, UINT64_MAX);
}
ZTEST_SUITE(task_manager, NULL, NULL, NULL, NULL, NULL);
