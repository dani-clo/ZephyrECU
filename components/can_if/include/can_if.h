#ifndef ZEPHYRECU_CAN_IF_H_
#define ZEPHYRECU_CAN_IF_H_

#include <stdbool.h>
#include <stdint.h>

/* Classical CAN data frames only: no RTR, CAN FD or error frames. */
typedef struct {
	uint32_t id;
	uint8_t dlc;
	bool extended;
	uint8_t data[8];
} can_if_frame_t;

typedef enum {
	CAN_IF_STOPPED,
	CAN_IF_ERROR_ACTIVE,
	CAN_IF_ERROR_WARNING,
	CAN_IF_ERROR_PASSIVE,
	CAN_IF_BUS_OFF,
	CAN_IF_STATE_UNKNOWN,
} can_if_state_t;

/* Dispatcher thread context. Frame is borrowed until callback returns. */
typedef void (*can_if_rx_callback_t)(const can_if_frame_t *frame, void *context);
/* Driver completion context (possibly ISR). Must not block or call send(). */
typedef void (*can_if_tx_callback_t)(int status, void *context);

typedef struct {
	uint32_t rx_queued;
	uint32_t rx_dispatched;
	uint32_t rx_callbacks;
	uint32_t rx_dropped;
	uint32_t rx_invalid;
	uint32_t rx_unmatched;
	uint32_t tx_submitted;
	uint32_t tx_completed;
	uint32_t tx_submit_errors;
	uint32_t tx_completion_errors;
	uint32_t tx_busy;
	uint32_t state_changes;
	uint32_t bus_off;
	bool tx_pending;
	int last_tx_error;
	can_if_state_t state;
	uint8_t tx_error_count;
	uint8_t rx_error_count;
} can_if_stats_t;

/* Thread-only, nonblocking lifecycle: init -> register all routes -> start.
 * init/start return -EALREADY after success. Registration is frozen on start.
 * Lifecycle operations may return -EBUSY if another operation is in progress.
 */
int can_if_init(void);
/* Exact ID only. Same ID/type shares one HW filter; identical cb/context is
 * rejected with -EALREADY. Callback/context remain valid for application life.
 */
int can_if_register_rx(uint32_t id, bool extended,
		       can_if_rx_callback_t callback, void *context);
int can_if_start(void);
/* Thread-only. 0 means submitted, not transmitted. The frame is copied.
 * -EAGAIN means one TX/submission is already in progress; retry policy belongs
 * to the caller. No internal retries. Optional callback reports completion.
 * On submission error there is no callback. Context must survive completion,
 * which can occur before send() returns. See docs/can-if.md for full contract.
 */
int can_if_send(const can_if_frame_t *frame, can_if_tx_callback_t callback, void *context);
/* Independently atomic, wrapping counters; not a transactional snapshot. */
int can_if_get_stats(can_if_stats_t *stats);

#endif
