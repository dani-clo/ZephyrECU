#include "app_diag.h"
#include "app_led.h"
#include "diag_uds.h"

#include <string.h>

static diag_uds_result_t read_identity(uint8_t *output, size_t capacity,
				      size_t *length, void *context)
{
	static const uint8_t identity[] = "ZephyrECU-UDS-01";

	(void)context;
	if (capacity < sizeof(identity) - 1) {
		return DIAG_UDS_RESPONSE_TOO_LONG;
	}
	memcpy(output, identity, sizeof(identity) - 1);
	*length = sizeof(identity) - 1;
	return DIAG_UDS_OK;
}

static diag_uds_result_t read_session(uint8_t *output, size_t capacity,
				     size_t *length, void *context)
{
	(void)context;
	if (capacity < 1) {
		return DIAG_UDS_RESPONSE_TOO_LONG;
	}
	diag_uds_stats_t stats;

	diag_uds_get_stats(&stats);
	output[0] = stats.session;
	*length = 1;
	return DIAG_UDS_OK;
}

static diag_uds_result_t led_routine(uint8_t control, const uint8_t *input,
	size_t input_length, uint8_t *output, size_t capacity, size_t *length, void *context)
{
	(void)input;
	(void)context;
	if (control != 1 && control != 3) {
		return DIAG_UDS_SUBFUNCTION_NOT_SUPPORTED;
	}
	if (input_length != 0) {
		return DIAG_UDS_INVALID_LENGTH;
	}
	if (capacity < 1) {
		return DIAG_UDS_RESPONSE_TOO_LONG;
	}
	if (control == 1 && app_led_diag_set(!app_led_diag_get()) != 0) {
		return DIAG_UDS_CONDITIONS_NOT_CORRECT;
	}
	output[0] = app_led_diag_get();
	*length = 1;
	return DIAG_UDS_OK;
}

int app_diag_register(void)
{
	int ret = diag_uds_register_did(0xf190, read_identity, NULL);

	if (ret == 0) {
		ret = diag_uds_register_did(0xf186, read_session, NULL);
	}
	if (ret == 0) {
		ret = diag_uds_register_routine(0xf001, led_routine, NULL);
	}
	return ret;
}
