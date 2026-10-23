#include "app_led.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zephyrecu);

int main(void)
{
	int ret = app_led_init();

	if (ret != 0) {
		LOG_ERR("LED initialization failed: %d", ret);
		return ret;
	}
	LOG_INF("ZephyrECU bootstrap ready");
	return 0;
}
