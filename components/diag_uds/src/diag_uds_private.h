#ifndef ZEPHYRECU_DIAG_UDS_PRIVATE_H_
#define ZEPHYRECU_DIAG_UDS_PRIVATE_H_

#include "diag_uds.h"
#include <zephyr/sys/atomic.h>
#include <stdbool.h>

/* Only atomics cross the driver completion / worker boundary. */
extern atomic_t diag_tx_submit_errors;
extern atomic_t diag_tx_busy;
extern atomic_t diag_tx_completed;
extern atomic_t diag_tx_completion_errors;
extern atomic_t diag_last_tx_error;

uint32_t diag_port_us_from_ticks(int64_t ticks);
int diag_port_last_submit(void);
void diag_port_poll(void);
void diag_port_abort(void);
bool diag_port_fc_pending(void);

#endif
