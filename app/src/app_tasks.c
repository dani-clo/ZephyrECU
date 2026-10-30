#include "app_tasks.h"
#include "app_led.h"
#include "can_comm.h"
#include "can_signals.h"
#include "ecu_task_manager.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_tasks);
#define ECU_EVENT_CAN_RX BIT(0)

static void led_main(uint32_t events);
ECU_TASK_DEFINE(app_led_task, led_main, 100);

/* Dispatcher owns the borrowed sample; flags carry no signal payload. */
static void on_led_command(can_signal_id_t signal,
			   const can_comm_sample_t *sample, void *context)
{
	ARG_UNUSED(signal);
	ARG_UNUSED(sample);
	ARG_UNUSED(context);
	(void)ecu_task_notify(app_led_task, ECU_EVENT_CAN_RX);
}

static void led_main(uint32_t events)
{
	ARG_UNUSED(events);
	can_comm_sample_t sample;

	/* Getter copies the latest sample under can_comm's cache mutex. Events
	 * coalesce: process current state, not an assumed history of CAN values.
	 * Periodic refresh also handles RX expiry, setting the LED safely off.
	 */
	int ret = can_comm_get_signal(CAN_SIGNAL_LED_COMMAND_LED_REQUEST, &sample);

	if (ret == 0) {
		bool on = sample.valid && sample.raw != 0;

		if (on != app_led_get()) { ret = app_led_set(on); }
	}
	if (ret != 0) { LOG_ERR("LED task update failed: %d", ret); }
}

static void monitor_main(uint32_t events)
{
	ARG_UNUSED(events);
	static uint8_t counter;

	/* Update cached status only; can_comm's independent scheduler sends it. */
	(void)can_signal_status_periodic_counter_set_raw(++counter);
	(void)can_signal_status_periodic_led_status_set_raw(app_led_get());
}
ECU_TASK_DEFINE(app_monitor_task, monitor_main, 10);

int app_tasks_init(void)
{
	return can_comm_register_signal_callback(CAN_SIGNAL_LED_COMMAND_LED_REQUEST,
						on_led_command, NULL);
}
