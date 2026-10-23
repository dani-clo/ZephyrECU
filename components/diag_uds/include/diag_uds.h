#ifndef ZEPHYRECU_DIAG_UDS_H_
#define ZEPHYRECU_DIAG_UDS_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
	DIAG_UDS_OK = 0,
	DIAG_UDS_GENERAL_REJECT = 0x10,
	DIAG_UDS_SUBFUNCTION_NOT_SUPPORTED = 0x12,
	DIAG_UDS_INVALID_LENGTH = 0x13,
	DIAG_UDS_RESPONSE_TOO_LONG = 0x14,
	DIAG_UDS_CONDITIONS_NOT_CORRECT = 0x22,
	DIAG_UDS_REQUEST_OUT_OF_RANGE = 0x31,
} diag_uds_result_t;

/* Synchronous UDS-thread handlers: brief, bounded, no protocol/driver access.
 * Write <= capacity bytes and set *length on success. Buffers are borrowed.
 * Slow/asynchronous operations and NRC 0x78 are not supported by this API.
 */
typedef diag_uds_result_t (*diag_uds_did_handler_t)(uint8_t *output, size_t capacity,
						 size_t *length, void *context);
typedef diag_uds_result_t (*diag_uds_routine_handler_t)(uint8_t control,
	const uint8_t *input, size_t input_length, uint8_t *output, size_t capacity,
	size_t *length, void *context);

typedef struct {
	uint32_t rx_queued;
	uint32_t rx_dropped;
	uint32_t rx_invalid;
	uint32_t rx_busy;
	uint32_t requests;
	uint32_t rejected;
	uint32_t responses_started;
	uint32_t suppressed;
	uint32_t transport_errors;
	uint32_t handler_errors;
	uint32_t session_timeouts;
	uint32_t poll_count;
	uint32_t max_poll_gap_us;
	uint32_t poll_deadline_misses;
	uint32_t tx_submit_errors;
	uint32_t tx_busy;
	uint32_t tx_completed;
	uint32_t tx_completion_errors;
	int last_tx_error;
	uint8_t session;
} diag_uds_stats_t;

/* Thread-only, one-shot lifecycle: can_if_init -> diag_uds_init -> register
 * handlers -> can_if_start -> diag_uds_start. Registration freezes at start.
 * init installs physical/functional RX routes; it does not start CAN.
 * The UDS worker alone initializes and mutates library state.
 */
int diag_uds_init(void);
int diag_uds_register_did(uint16_t did, diag_uds_did_handler_t handler, void *context);
int diag_uds_register_routine(uint16_t rid, diag_uds_routine_handler_t handler, void *context);
int diag_uds_start(void);
/* Independently atomic, wrapping counters; handler contexts remain valid for life. */
int diag_uds_get_stats(diag_uds_stats_t *stats);

#endif
