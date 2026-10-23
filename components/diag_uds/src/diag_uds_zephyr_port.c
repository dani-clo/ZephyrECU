#include "can_if.h"
#include "diag_uds_private.h"
#include "iso14229.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

atomic_t diag_tx_submit_errors;
atomic_t diag_tx_busy;
atomic_t diag_tx_completed;
atomic_t diag_tx_completion_errors;
atomic_t diag_last_tx_error;
/* Worker-only: retain a failed FlowControl until accepted or N_Bs expires.
 * Upstream checks CF backpressure, but ignores the FlowControl send result.
 */
static int last_submit;
static can_if_frame_t pending_fc;
static bool fc_pending;
static uint32_t fc_deadline;

int diag_port_last_submit(void)
{
	return last_submit;
}

bool diag_port_fc_pending(void)
{
	return fc_pending;
}

uint32_t diag_port_us_from_ticks(int64_t ticks)
{
	return (uint32_t)k_ticks_to_us_floor64(ticks);
}

uint32_t isotp_user_get_us(void)
{
	/* Absolute tick-derived time; conversion in 64 bits before natural u32 wrap. */
	return diag_port_us_from_ticks(k_uptime_ticks());
}

uint32_t UDSMillis(void)
{
	return (uint32_t)k_uptime_get();
}

void isotp_user_debug(const char *message, ...)
{
	/* Protocol/result counters are sampled by the UDS worker; no hot-path logs. */
	ARG_UNUSED(message);
}

static void tx_done(int status, void *context)
{
	ARG_UNUSED(context);
	if (status == 0) {
		atomic_inc(&diag_tx_completed);
	} else {
		atomic_inc(&diag_tx_completion_errors);
		atomic_set(&diag_last_tx_error, status);
	}
}

int isotp_user_send_can(uint32_t id, const uint8_t *data, uint8_t size, void *context)
{
	ARG_UNUSED(context);
	if (data == NULL || size > 8 || id != CONFIG_DIAG_UDS_PHYSICAL_TX_ID) {
		atomic_inc(&diag_tx_submit_errors);
		atomic_set(&diag_last_tx_error, -EINVAL);
		last_submit = -EINVAL;
		return ISOTP_RET_ERROR;
	}
	can_if_frame_t frame = {.id = id, .dlc = size};

	memcpy(frame.data, data, size);
	int ret = can_if_send(&frame, tx_done, NULL);

	last_submit = ret;

	if (ret == -EAGAIN) {
		if (size >= 3 && (data[0] >> 4) == 3 && !fc_pending) {
			pending_fc = frame;
			fc_pending = true;
			fc_deadline = UDSMillis() + ISO_TP_DEFAULT_RESPONSE_TIMEOUT_US / 1000;
		}
		atomic_inc(&diag_tx_busy);
		atomic_set(&diag_last_tx_error, ret);
		return ISOTP_RET_NOSPACE;
	}
	if (ret != 0) {
		atomic_inc(&diag_tx_submit_errors);
		atomic_set(&diag_last_tx_error, ret);
		return ISOTP_RET_ERROR;
	}
	/* Accepted by can_if. Only tx_done reports actual driver completion. */
	return ISOTP_RET_OK;
}

void diag_port_abort(void)
{
	fc_pending = false;
}

void diag_port_poll(void)
{
	if (!fc_pending) {
		return;
	}
	if (UDSTimeAfter(UDSMillis(), fc_deadline)) {
		fc_pending = false;
		atomic_inc(&diag_tx_submit_errors);
		atomic_set(&diag_last_tx_error, -ETIMEDOUT);
		return;
	}
	int ret = isotp_user_send_can(pending_fc.id, pending_fc.data, pending_fc.dlc, NULL);

	if (ret != ISOTP_RET_NOSPACE) {
		fc_pending = false;
	}
}
