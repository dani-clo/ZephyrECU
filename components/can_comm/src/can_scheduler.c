#include "can_comm_private.h"
#include "can_if.h"

#include <errno.h>
#include <string.h>

static unsigned int cursor;

static void completed(int status, void *context)
{
	struct can_comm_pdu_state *state = context;

	/* Driver/ISR context, including inline completion before send returns. */
	atomic_set(&state->completion_status, status);
	atomic_set(&state->completion_ready, 1);
}

void can_comm_schedule(int64_t now)
{
	unsigned int first = cursor;

	for (unsigned int offset = 0; offset < CAN_PDU_COUNT; ++offset) {
		unsigned int id = (first + offset) % CAN_PDU_COUNT;
		const struct can_pdu_cfg *cfg = &can_pdu_config[id];
		struct can_comm_pdu_state *state = &can_comm_pdus[id];

		k_mutex_lock(&can_comm_lock, K_FOREVER);
		if (cfg->mode == CAN_COMM_RX) {
			if (state->valid && now - state->received_at >= cfg->timeout_ms) {
				state->valid = false;
				++can_comm_stats.rx_timeouts;
			}
			k_mutex_unlock(&can_comm_lock);
			continue;
		}
		if (atomic_cas(&state->completion_ready, 1, 0)) {
			state->in_flight = false;
			if (atomic_get(&state->completion_status) == 0) {
				++can_comm_stats.tx_completed;
			} else {
				++can_comm_stats.tx_completion_errors;
			}
		}
		if (cfg->period_ms != 0 && now >= state->next_periodic) {
			state->pending_periodic = true;
			/* Skip missed deadlines without a burst or moving the base phase. */
			state->next_periodic += ((now - state->next_periodic) / cfg->period_ms + 1) * cfg->period_ms;
		}
		if (state->in_flight || (!state->pending_trigger && !state->pending_periodic) ||
		    (state->attempted && now - state->last_attempt < cfg->min_delay_ms)) {
			k_mutex_unlock(&can_comm_lock);
			continue;
		}
		can_if_frame_t frame = {.id = cfg->id, .dlc = cfg->dlc, .extended = cfg->extended};
		bool event = state->pending_trigger, periodic = state->pending_periodic;

		memcpy(frame.data, state->data, sizeof(frame.data));
		state->pending_trigger = false;
		state->pending_periodic = false;
		state->in_flight = true;
		k_mutex_unlock(&can_comm_lock);
		/* Never hold cache lock across driver code or an inline completion. A new
		 * setter/trigger during submission remains pending for the next send.
		 */
		int ret = can_if_send(&frame, completed, state);

		k_mutex_lock(&can_comm_lock, K_FOREVER);
		if (ret == -EAGAIN) {
			state->pending_trigger |= event;
			state->pending_periodic |= periodic;
			state->in_flight = false;
			++can_comm_stats.tx_busy;
		} else {
			state->attempted = true;
			state->last_attempt = now;
			if (ret == 0) {
				++can_comm_stats.tx_submitted;
			} else {
				state->in_flight = false;
				++can_comm_stats.tx_submit_errors;
			}
		}
		k_mutex_unlock(&can_comm_lock);
	}
	cursor = (first + 1) % CAN_PDU_COUNT;
}

static void worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		can_comm_schedule(k_uptime_get());
		k_sleep(K_MSEC(1));
	}
}

K_THREAD_DEFINE(comm_id, CONFIG_CAN_COMM_STACK_SIZE, worker, NULL, NULL, NULL,
		CONFIG_CAN_COMM_PRIORITY, 0, SYS_FOREVER_MS);

void can_comm_scheduler_start(int64_t now)
{
	for (unsigned int i = 0; i < CAN_PDU_COUNT; ++i) {
		can_comm_pdus[i].next_periodic = now + can_pdu_config[i].period_ms;
	}
	k_thread_start(comm_id);
}
