#ifndef ZEPHYRECU_CAN_COMM_H_
#define ZEPHYRECU_CAN_COMM_H_

#include "can_cfg.h"

typedef struct {
	int64_t raw;
	double physical;
	int64_t age_ms;
	bool received;
	bool valid;
} can_comm_sample_t;

/* Immutable reception snapshot. raw[] follows this PDU's generated signal order.
 * Only signal_count entries and dlc bytes are meaningful. Borrowed in callbacks.
 */
typedef struct {
	can_pdu_id_t pdu;
	int64_t timestamp_ms;
	int64_t raw[CAN_COMM_MAX_PDU_SIGNALS];
	uint8_t data[8];
} can_comm_pdu_sample_t;

/* can_if dispatcher thread; never ISR. Keep callbacks brief. Cache is fully
 * updated before callbacks, which may call getters, setters and trigger_tx().
 */
typedef void (*can_comm_pdu_rx_cb_t)(const can_comm_pdu_sample_t *sample, void *context);
typedef void (*can_comm_signal_cb_t)(can_signal_id_t signal,
				    const can_comm_sample_t *sample, void *context);

typedef struct {
	uint32_t rx_pdus, rx_invalid, rx_timeouts;
	uint32_t tx_submitted, tx_completed, tx_submit_errors, tx_completion_errors, tx_busy;
} can_comm_stats_t;

/* Thread-only: can_if_init -> can_comm_init -> register callbacks ->
 * can_if_start -> can_comm_start. No allocation; no stop/reinitialization API.
 * init can be retried after route registration failure. Registration freezes
 * on start. Contexts live for the application lifetime.
 */
int can_comm_init(void);
int can_comm_start(void);
int can_comm_register_pdu_rx_callback(can_pdu_id_t pdu, can_comm_pdu_rx_cb_t cb, void *context);
int can_comm_register_signal_callback(can_signal_id_t signal, can_comm_signal_cb_t cb, void *context);
/* 0 on success even before reception or after expiry: inspect received/valid.
 * Raw is unscaled integer; physical = raw * DBC scale + offset. TX samples are
 * valid initialized cache values, not evidence of successful bus transmission.
 */
int can_comm_get_signal(can_signal_id_t signal, can_comm_sample_t *sample);
/* TX only. Enforces DBC raw/physical bounds. Does not set RX cache values. */
int can_comm_set_signal_raw(can_signal_id_t signal, int64_t raw);
/* Coalesced whole-PDU request. PERIODIC and RX PDUs return -EACCES. */
int can_comm_trigger_tx(can_pdu_id_t pdu);
int can_comm_get_stats(can_comm_stats_t *stats);

#endif
