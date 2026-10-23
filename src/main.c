#include "app_led.h"
#include "app_diag.h"
#include "diag_uds.h"
#include "can_if.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zephyrecu);

int main(void)
{
	int ret = app_led_init();

	if (ret != 0) {
		LOG_ERR("LED initialization failed: %d", ret);
		return ret;
	}
	ret = can_if_init();
	if (ret != 0) {
		LOG_ERR("CAN interface initialization failed: %d", ret);
		return ret;
	}
	ret = diag_uds_init();
	if (ret == 0) {
		ret = app_diag_register();
	}
	if (ret != 0) {
		LOG_ERR("Diagnostic registration failed: %d", ret);
		return ret;
	}
	ret = can_if_start();
	if (ret != 0) {
		LOG_ERR("CAN interface start failed: %d", ret);
		return ret;
	}
	ret = diag_uds_start();
	if (ret != 0) {
		LOG_ERR("Diagnostic worker start failed: %d", ret);
		return ret;
	}
	LOG_INF("ZephyrECU CAN and diagnostics ready");
	return 0;
}
