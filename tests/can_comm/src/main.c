#include "can_comm.h"
#include "can_comm_private.h"
#include "can_if.h"
#include "can_signals.h"

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;
extern const k_tid_t comm_id;
static const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static can_rx_callback_t ingress;
static struct can_frame injection;
static atomic_t pdu_notifications, signal_notifications;
static int64_t clock_base, test_now;
static struct {
	int64_t at;
	struct can_frame frame;
} sent[256];
static unsigned int sent_count;
static bool hold_next, fresh_during_send;
static int submit_error, completion_error;
static bool fail_extended_route;
static can_tx_callback_t held_callback;
static void *held_context;

static int add_filter(const struct device *device, can_rx_callback_t callback,
		      void *context, const struct can_filter *filter)
{
	ARG_UNUSED(device);
	ARG_UNUSED(context);
	ingress = callback;
	zassert_equal(filter->mask, filter->flags ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK);
	if (fail_extended_route && filter->flags != 0) { return -ENOSPC; }
	return (int)fake_can_add_rx_filter_fake.call_count;
}

static int send_frame(const struct device *device, const struct can_frame *frame,
		      k_timeout_t timeout, can_tx_callback_t callback, void *context)
{
	ARG_UNUSED(timeout);
	zassert_true(sent_count < ARRAY_SIZE(sent));
	sent[sent_count].at = test_now - clock_base;
	sent[sent_count++].frame = *frame;
	if (fresh_during_send && frame->id == 0x221) {
		fresh_during_send = false;
		zassert_ok(can_signal_status_event_event_value_set_raw(9));
	}
	if (submit_error != 0) {
		int ret = submit_error;

		submit_error = 0;
		return ret;
	}
	if (hold_next) {
		hold_next = false;
		held_callback = callback;
		held_context = context;
	} else {
		callback(device, completion_error, context);
		completion_error = 0;
	}
	return 0;
}

static void at(int64_t ms)
{
	test_now = clock_base + ms;
	can_comm_schedule(test_now);
}

static unsigned int count(uint32_t id)
{
	unsigned int found = 0;

	for (unsigned int i = 0; i < sent_count; ++i) {
		found += sent[i].frame.id == id;
	}
	return found;
}

static unsigned int last(uint32_t id)
{
	for (unsigned int i = sent_count; i > 0; --i) {
		if (sent[i - 1].frame.id == id) { return i - 1; }
	}
	zassert_unreachable("Expected TX ID %x", id);
	return 0;
}

static can_comm_stats_t stats(void)
{
	can_comm_stats_t result;

	zassert_ok(can_comm_get_stats(&result));
	return result;
}

static void pdu_rx(const can_comm_pdu_sample_t *pdu, void *context)
{
	ARG_UNUSED(context);
	zassert_false(k_is_in_isr());
	zassert_equal(pdu->pdu, CAN_PDU_LED_COMMAND);
	can_comm_sample_t demand, led;

	zassert_ok(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_DEMAND, &demand));
	zassert_ok(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, &led));
	zassert_equal(demand.raw, pdu->raw[0]);
	zassert_equal(led.raw, pdu->raw[1]);
	zassert_true(demand.valid && led.valid);
	zassert_equal(pdu->data[0], led.raw);
	atomic_inc(&pdu_notifications);
}

static void signal_rx(can_signal_id_t signal, const can_comm_sample_t *sample, void *context)
{
	ARG_UNUSED(context);
	zassert_false(k_is_in_isr());
	zassert_equal(signal, CAN_SIGNAL_LED_COMMAND_LED_REQUEST);
	zassert_true(sample->valid);
	/* Setter from the RX callback must not deadlock the cache. */
	zassert_ok(can_signal_status_periodic_led_status_set_raw((uint8_t)sample->raw));
	atomic_inc(&signal_notifications);
}

static void lifecycle(void)
{
	can_comm_sample_t sample;

	zassert_equal(can_comm_start(), -EACCES);
	zassert_equal(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_DEMAND, &sample), -EACCES);
	zassert_equal(can_comm_init(), -EACCES);
	zassert_ok(can_if_init());
	fake_can_add_rx_filter_fake.custom_fake = add_filter;
	fail_extended_route = true;
	zassert_equal(can_comm_init(), -ENOSPC);
	fail_extended_route = false;
	zassert_ok(can_comm_init());
	zassert_equal(can_comm_init(), -EALREADY);
	zassert_equal(can_comm_start(), -EACCES); /* CAN controller still stopped */
	zassert_equal(fake_can_add_rx_filter_fake.call_count, 3); /* retry only failed second route */
	zassert_ok(can_comm_register_pdu_rx_callback(CAN_PDU_LED_COMMAND, pdu_rx, NULL));
	zassert_equal(can_comm_register_pdu_rx_callback(CAN_PDU_LED_COMMAND, pdu_rx, NULL), -EALREADY);
	zassert_equal(can_comm_register_pdu_rx_callback(CAN_PDU_LED_COMMAND, pdu_rx, &sent), -ENOSPC);
	zassert_ok(can_comm_register_signal_callback(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, signal_rx, NULL));
	zassert_equal(can_comm_register_signal_callback(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, signal_rx, NULL), -EALREADY);
	zassert_equal(can_comm_register_signal_callback(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, signal_rx, &sent), -ENOSPC);
	zassert_equal(can_comm_register_pdu_rx_callback(CAN_PDU_STATUS_EVENT, pdu_rx, NULL), -EACCES);
	zassert_equal(can_comm_register_signal_callback(CAN_SIGNAL_STATUS_EVENT_EVENT_COUNT, signal_rx, NULL), -EACCES);
	zassert_equal(can_comm_get_signal(CAN_SIGNAL_COUNT, &sample), -EINVAL);
	zassert_equal(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_DEMAND, NULL), -EINVAL);
	zassert_equal(can_comm_set_signal_raw(CAN_SIGNAL_LED_COMMAND_DEMAND, 1), -EACCES);
	zassert_equal(can_signal_status_periodic_led_status_set_raw(2), -ERANGE);
	zassert_equal(can_comm_trigger_tx(CAN_PDU_STATUS_PERIODIC), -EACCES);
	zassert_equal(can_comm_trigger_tx(CAN_PDU_LED_COMMAND), -EACCES);
	zassert_ok(can_if_start());
	fake_can_send_fake.custom_fake = send_frame;
	zassert_ok(can_comm_start());
	/* Drive the real scheduler with explicit times; no test-only runtime hook. */
	k_thread_suspend(comm_id);
	clock_base = can_comm_pdus[CAN_PDU_STATUS_PERIODIC].next_periodic - 40;
	zassert_equal(can_comm_start(), -EALREADY);
	zassert_equal(can_comm_register_pdu_rx_callback(CAN_PDU_LED_COMMAND, pdu_rx, NULL), -EBUSY);
	zassert_equal(can_comm_register_signal_callback(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, signal_rx, NULL), -EBUSY);
}

static void scheduling(void)
{
	zassert_ok(can_signal_status_periodic_led_status_set_raw(1));
	zassert_ok(can_signal_status_periodic_counter_set_raw(0xab));
	at(0);
	zassert_equal(sent_count, 0);
	at(39);
	zassert_equal(sent_count, 0);
	at(40);
	zassert_equal(count(0x220), 1);
	zassert_mem_equal(sent[last(0x220)].frame.data, ((uint8_t[]){1, 0xab}), 2);
	zassert_equal(sent[last(0x220)].frame.dlc, 2);
	zassert_equal(sent[last(0x220)].frame.flags, 0);
	zassert_ok(can_signal_status_event_event_value_set_raw(1));
	zassert_ok(can_signal_status_event_event_value_set_raw(2));
	zassert_ok(can_signal_status_event_event_value_set_raw(3));
	at(41);
	zassert_equal(count(0x221), 1);
	zassert_mem_equal(sent[last(0x221)].frame.data, ((uint8_t[]){0, 3, 0}), 3);
	zassert_ok(can_signal_status_event_event_value_set_raw(3));
	zassert_ok(can_signal_status_event_event_count_set_raw(7));
	at(42);
	zassert_equal(count(0x221), 1); /* unchanged / PENDING does not trigger */
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_EVENT));
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_EVENT));
	at(55);
	zassert_equal(count(0x221), 1);
	at(56);
	zassert_equal(count(0x221), 2);
	zassert_mem_equal(sent[last(0x221)].frame.data, ((uint8_t[]){0, 3, 7}), 3);

	/* An event at 70 must not move the MIXED periodic deadline at 100. */
	zassert_ok(can_signal_status_mixed_mixed_value_set_raw(0xabc));
	at(70);
	zassert_equal(count(0x222), 1);
	zassert_mem_equal(sent[last(0x222)].frame.data, ((uint8_t[]){0xbc, 0x0a, 0}), 3);
	at(80);
	zassert_equal(count(0x220), 2);
	at(99);
	zassert_equal(count(0x222), 1);
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_MIXED));
	at(100);
	zassert_equal(count(0x222), 2); /* due periodic + event combined */
	at(101);
	zassert_equal(count(0x222), 2);
	at(350); /* skip missed 200/300 deadlines, one coherent latest snapshot */
	zassert_equal(count(0x222), 3);
	at(399);
	zassert_equal(count(0x222), 3);
	at(400);
	zassert_equal(count(0x222), 4);

	/* Driver busy: retain newest data until the previous transmission completes. */
	hold_next = true;
	zassert_ok(can_signal_status_event_event_value_set_raw(-123));
	at(401);
	zassert_not_null(held_callback);
	zassert_mem_equal(sent[last(0x221)].frame.data, ((uint8_t[]){0xff, 0x85, 7}), 3);
	zassert_ok(can_signal_status_event_event_value_set_raw(5));
	zassert_ok(can_signal_status_event_event_value_set_raw(6));
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_MIXED));
	at(425);
	zassert_true(stats().tx_busy > 0);
	unsigned int events = count(0x221);

	held_callback(dev, 0, held_context);
	held_callback = NULL;
	at(426);
	zassert_equal(count(0x221), events + 1);
	zassert_mem_equal(sent[last(0x221)].frame.data, ((uint8_t[]){0, 6, 7}), 3);
	at(427);
	zassert_equal(count(0x221), events + 1);

	/* Fresh event during send(), including inline completion, must survive. */
	fresh_during_send = true;
	zassert_ok(can_signal_status_event_event_value_set_raw(8));
	at(441);
	zassert_equal(sent[last(0x221)].frame.data[1], 8);
	at(455);
	zassert_equal(sent[last(0x221)].frame.data[1], 8);
	at(456);
	zassert_equal(sent[last(0x221)].frame.data[1], 9);
	at(457);

	/* Hard failures consume the attempt; only a fresh trigger retries. */
	uint32_t errors = stats().tx_submit_errors;

	submit_error = -EIO;
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_EVENT));
	at(471);
	zassert_equal(stats().tx_submit_errors, errors + 1);
	events = count(0x221);
	at(486);
	zassert_equal(count(0x221), events);
	completion_error = -EIO;
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_EVENT));
	at(487);
	at(488);
	zassert_equal(stats().tx_completion_errors, 1);
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_MIXED));
	at(491);
	unsigned int mixed = count(0x222);

	at(499);
	zassert_equal(count(0x221), events + 1);
	zassert_ok(can_comm_trigger_tx(CAN_PDU_STATUS_EVENT));
	at(502);
	at(503);
	zassert_equal(count(0x221), events + 2);
	zassert_equal(stats().tx_submit_errors, 1);
	zassert_true(stats().tx_completed > 0);
	zassert_equal(count(0x222), mixed); /* deadline 500 held by min-delay */
	at(510);
	zassert_equal(count(0x222), mixed);
	at(511);
	zassert_equal(count(0x222), mixed + 1);
	at(599);
	zassert_equal(count(0x222), mixed + 1);
	at(600);
	zassert_equal(count(0x222), mixed + 2); /* phase survives delayed send */
}

static void inject_irq(const void *unused)
{
	ARG_UNUSED(unused);
	ingress(dev, &injection, NULL);
}

static void inject(uint32_t id, bool extended, uint8_t dlc, const uint8_t data[8])
{
	can_if_stats_t before, current;

	zassert_ok(can_if_get_stats(&before));
	injection = (struct can_frame){.id = id, .flags = extended ? CAN_FRAME_IDE : 0, .dlc = dlc};
	memcpy(injection.data, data, 8);
	irq_offload(inject_irq, NULL);
	memset(&injection, 0xa5, sizeof(injection));
	int64_t deadline = k_uptime_get() + 20;

	do {
		zassert_ok(can_if_get_stats(&current));
		if (current.rx_dispatched > before.rx_dispatched) { return; }
		k_sleep(K_MSEC(1));
	} while (k_uptime_get() < deadline);
	zassert_unreachable("RX dispatcher did not deliver frame");
}

static void reception(void)
{
	can_comm_sample_t sample;

	zassert_ok(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, &sample));
	zassert_false(sample.valid);
	zassert_false(sample.received);
	k_thread_resume(comm_id);
	inject(0x120, false, 4, (uint8_t[]){1, 0xf8, 0x50, 0, 0, 0, 0, 0});
	zassert_equal(atomic_get(&pdu_notifications), 1);
	zassert_equal(atomic_get(&signal_notifications), 1);
	zassert_ok(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_DEMAND, &sample));
	zassert_equal(sample.raw, -123);
	zassert_within(sample.physical, -12.3, 0.00001);
	inject(0x120, false, 4, (uint8_t[]){1, 0xf8, 0x50, 0, 0, 0, 0, 0});
	zassert_equal(atomic_get(&pdu_notifications), 2);
	zassert_equal(atomic_get(&signal_notifications), 1);
	inject(0x120, false, 4, (uint8_t[]){0, 0xf8, 0x50, 0, 0, 0, 0, 0});
	zassert_equal(atomic_get(&signal_notifications), 2);
	inject(0x18ff0101, true, 8, (uint8_t[]){0xff, 0xff, 0xff, 0xff, 0x80, 0, 0, 0});
	zassert_ok(can_comm_get_signal(CAN_SIGNAL_VEHICLE_FEEDBACK_COUNTER, &sample));
	zassert_equal(sample.raw, UINT32_MAX);
	zassert_ok(can_comm_get_signal(CAN_SIGNAL_VEHICLE_FEEDBACK_TEMPERATURE, &sample));
	zassert_equal(sample.raw, -2048);
	zassert_within(sample.physical, -1064, 0.00001);
	uint32_t invalid = stats().rx_invalid;

	inject(0x120, false, 3, (uint8_t[]){1, 0, 0, 0, 0, 0, 0, 0});
	zassert_equal(stats().rx_invalid, invalid + 1);
	inject(0x120, false, 4, (uint8_t[]){1, 0x80, 0, 0, 0, 0, 0, 0});
	zassert_equal(stats().rx_invalid, invalid + 2); /* raw outside DBC physical bounds */
	zassert_equal(atomic_get(&pdu_notifications), 3);
	k_sleep(K_MSEC(65));
	zassert_ok(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_DEMAND, &sample));
	zassert_true(sample.received);
	zassert_false(sample.valid);
	zassert_true(sample.age_ms >= 60);
	zassert_equal(sample.raw, -123); /* retain last value, explicitly stale */
	zassert_true(stats().rx_timeouts >= 1);
	inject(0x120, false, 4, (uint8_t[]){0, 0xf8, 0x50, 0, 0, 0, 0, 0});
	zassert_ok(can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_DEMAND, &sample));
	zassert_true(sample.valid);
	zassert_equal(atomic_get(&pdu_notifications), 4);
	zassert_equal(atomic_get(&signal_notifications), 2); /* revalidation is not raw change */
	uint8_t led;
	bool valid;

	zassert_ok(can_signal_led_command_led_request_get_raw(&led, &valid));
	zassert_equal(led, 0);
	zassert_true(valid);
	zassert_equal(can_signal_led_command_led_request_get_raw(NULL, &valid), -EINVAL);
	zassert_equal(can_comm_get_stats(NULL), -EINVAL);
}

ZTEST(can_comm, test_contract)
{
	lifecycle();
	scheduling();
	reception();
}

ZTEST_SUITE(can_comm, NULL, NULL, NULL, NULL, NULL);
