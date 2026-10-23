/* Test-only raw CAN exerciser; deliberately slow callback tests queue overflow. */
#include "can_if.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

K_MSGQ_DEFINE(echo_queue, sizeof(can_if_frame_t), 64, 4);

static void echo_rx(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(context);
	printk("RX_A %x %u %u %02x\n", frame->id, frame->extended, frame->dlc,
	       frame->data[0]);
	if (k_msgq_put(&echo_queue, frame, K_NO_WAIT) != 0) {
		printk("TEST_ECHO_QUEUE_FULL\n");
	}
}

static void fanout_rx(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(context);
	printk("RX_B %x %u %u %02x\n", frame->id, frame->extended, frame->dlc,
	       frame->data[0]);
}

static void slow_rx(const can_if_frame_t *frame, void *context)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(context);
	printk("BLOCKED\n");
	k_sleep(K_MSEC(200));
	printk("UNBLOCKED\n");
}

int main(void)
{
	int ret = can_if_init();

	if (ret == 0) {
		ret = can_if_register_rx(0x123, false, echo_rx, NULL);
	}
	if (ret == 0) {
		ret = can_if_register_rx(0x123, false, fanout_rx, NULL);
	}
	if (ret == 0) {
		ret = can_if_register_rx(0x123, true, echo_rx, NULL);
	}
	if (ret == 0) {
		ret = can_if_register_rx(0x124, false, slow_rx, NULL);
	}
	if (ret == 0) {
		ret = can_if_start();
	}
	if (ret != 0) {
		printk("START_FAILED %d\n", ret);
		return ret;
	}
	printk("READY\n");
	while (true) {
		can_if_frame_t frame;

		if (k_msgq_get(&echo_queue, &frame, K_MSEC(50)) == 0) {
			frame.id = frame.extended ? 0x1abcde : 0x321;
			int64_t deadline = k_uptime_get() + 100;

			do {
				ret = can_if_send(&frame, NULL, NULL);
				if (ret != -EAGAIN) {
					break;
				}
				k_sleep(K_MSEC(1));
			} while (k_uptime_get() < deadline);
			if (ret != 0) {
				printk("TX_FAILED %d\n", ret);
			}
		}
		can_if_stats_t stats;

		can_if_get_stats(&stats);
		printk("STATS rx=%u callbacks=%u dropped=%u invalid=%u submitted=%u "
		       "completed=%u errors=%u pending=%u\n",
		       stats.rx_dispatched, stats.rx_callbacks, stats.rx_dropped,
		       stats.rx_invalid, stats.tx_submitted, stats.tx_completed,
		       stats.tx_submit_errors + stats.tx_completion_errors, stats.tx_pending);
	}
}
