#ifndef ZEPHYRECU_CAN_COMM_PRIVATE_H_
#define ZEPHYRECU_CAN_COMM_PRIVATE_H_

#include "can_comm.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct can_comm_pdu_state {
	uint8_t data[8];
	bool received, valid, pending_trigger, pending_periodic, in_flight, attempted;
	int64_t received_at, next_periodic, last_attempt;
	atomic_t completion_ready, completion_status;
};

extern struct k_mutex can_comm_lock;
extern struct can_comm_pdu_state can_comm_pdus[CAN_PDU_COUNT];
extern can_comm_stats_t can_comm_stats;
void can_comm_schedule(int64_t now);
void can_comm_scheduler_start(int64_t now);

#endif
