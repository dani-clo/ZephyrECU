#include "app_led.h"
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
	/* Future clients register their RX routes here before the single start. */
	ret = can_if_start();
	if (ret != 0) {
		LOG_ERR("CAN interface start failed: %d", ret);
		return ret;
	}
	LOG_INF("ZephyrECU CAN interface ready");
	return 0;
}
