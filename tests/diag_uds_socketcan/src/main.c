/* Real stack integration observer with independent raw application CAN traffic. */
#include "app_diag.h"
#include "app_led.h"
#include "can_if.h"
#include "diag_uds.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

K_MSGQ_DEFINE(application_queue, sizeof(can_if_frame_t), 16, 4);

static void application_rx(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(context);
	k_msgq_put(&application_queue, frame, K_NO_WAIT);
}

int main(void)
{
	int ret = app_led_init();

	if (ret == 0) { ret = can_if_init(); }
	if (ret == 0) { ret = diag_uds_init(); }
	if (ret == 0) { ret = app_diag_register(); }
	if (ret == 0) { ret = can_if_register_rx(0x123, false, application_rx, NULL); }
	if (ret == 0) { ret = can_if_start(); }
	if (ret == 0) { ret = diag_uds_start(); }
	if (ret != 0) {
		printk("START_FAILED %d\n", ret);
		return ret;
	}
	printk("READY\n");
	int64_t next_report = 0;
	uint8_t heartbeat = 0;

	while (true) {
		can_if_frame_t frame;

		if (k_msgq_get(&application_queue, &frame, K_MSEC(10)) == 0) {
			frame.id = 0x321;
			int64_t deadline = k_uptime_get() + 50;

			do {
				ret = can_if_send(&frame, NULL, NULL);
				if (ret != -EAGAIN) { break; }
				k_sleep(K_MSEC(1));
			} while (k_uptime_get() < deadline);
			if (ret != 0) { printk("APP_TX_FAILED %d\n", ret); }
		} else {
			frame = (can_if_frame_t){.id = 0x322, .dlc = 1, .data = {heartbeat++}};
			/* Best-effort independent traffic; skip if diagnostics own the slot. */
			can_if_send(&frame, NULL, NULL);
		}
		if (k_uptime_get() >= next_report) {
			diag_uds_stats_t stats;
			can_if_stats_t can_stats;

			diag_uds_get_stats(&stats);
			can_if_get_stats(&can_stats);
			printk("DIAG requests=%u rejected=%u responses=%u invalid=%u dropped=%u "
			       "transport_errors=%u completed=%u tx_errors=%u max_poll_us=%u "
			       "poll_misses=%u session=%u timeouts=%u can_drops=%u\n",
			       stats.requests, stats.rejected, stats.responses_started, stats.rx_invalid,
			       stats.rx_dropped, stats.transport_errors, stats.tx_completed,
			       stats.tx_submit_errors + stats.tx_completion_errors,
			       stats.max_poll_gap_us, stats.poll_deadline_misses, stats.session,
			       stats.session_timeouts, can_stats.rx_dropped);
			next_report = k_uptime_get() + 250;
		}
	}
}
