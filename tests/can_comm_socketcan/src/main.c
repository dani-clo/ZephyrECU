/* Focused integration exerciser. Application SWCs are intentionally separate. */
#include "can_comm.h"
#include "can_if.h"
#include "can_signals.h"
#include "diag_uds.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static void command(const can_comm_pdu_sample_t *snapshot, void *context)
{
	ARG_UNUSED(snapshot);
	ARG_UNUSED(context);
	static uint8_t counter;
	int16_t demand;
	uint8_t led;
	bool valid;

	can_signal_led_command_demand_get_raw(&demand, &valid);
	can_signal_led_command_led_request_get_raw(&led, &valid);
	int ret = can_signal_status_periodic_led_status_set_raw(led);

	ret |= can_signal_status_periodic_counter_set_raw(++counter);
	ret |= can_signal_status_event_event_count_set_raw(counter);
	ret |= can_signal_status_event_event_value_set_raw(demand);
	ret |= can_signal_status_mixed_mixed_value_set_raw(led);
	printk("COMMAND %d %u %u status=%d\n", demand, led, counter, ret);
}

static void changed(can_signal_id_t signal, const can_comm_sample_t *sample, void *context)
{
	ARG_UNUSED(signal);
	ARG_UNUSED(context);
	printk("CHANGED %lld\n", (long long)sample->raw);
}

static void feedback(const can_comm_pdu_sample_t *snapshot, void *context)
{
	ARG_UNUSED(snapshot);
	ARG_UNUSED(context);
	can_comm_sample_t counter, temperature;

	can_comm_get_signal(CAN_SIGNAL_VEHICLE_FEEDBACK_COUNTER, &counter);
	can_comm_get_signal(CAN_SIGNAL_VEHICLE_FEEDBACK_TEMPERATURE, &temperature);
	printk("FEEDBACK %lld %lld\n", (long long)counter.raw, (long long)temperature.raw);
}

int main(void)
{
	int ret = can_if_init();

	if (ret == 0) { ret = can_comm_init(); }
	if (ret == 0) { ret = diag_uds_init(); }
	if (ret == 0) { ret = can_comm_register_pdu_rx_callback(CAN_PDU_LED_COMMAND, command, NULL); }
	if (ret == 0) { ret = can_comm_register_pdu_rx_callback(CAN_PDU_VEHICLE_FEEDBACK, feedback, NULL); }
	if (ret == 0) { ret = can_comm_register_signal_callback(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, changed, NULL); }
	if (ret == 0) { ret = can_if_start(); }
	if (ret == 0) { ret = can_comm_start(); }
	if (ret == 0) { ret = diag_uds_start(); }
	if (ret != 0) {
		printk("START_FAILED %d\n", ret);
		return ret;
	}
	printk("COMM_READY\n");
	bool was_valid = false;

	while (true) {
		can_comm_sample_t value;

		can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, &value);
		if (value.received && value.valid != was_valid) {
			printk("VALID %u\n", value.valid);
			was_valid = value.valid;
		}
		k_sleep(K_MSEC(5));
	}
}
