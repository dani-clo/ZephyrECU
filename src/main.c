#include "app_led.h"
#include "app_diag.h"
#include "diag_uds.h"
#include "can_if.h"
#include "can_comm.h"
#include "app_tasks.h"
#include "ecu_task_manager.h"
#if defined(CONFIG_ECU_NVM)
#include "ecu_nvm.h"
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zephyrecu);

int main(void)
{
	int ret = app_led_init();

	if (ret != 0) {
		LOG_ERR("LED initialization failed: %d", ret);
		return ret;
	}
#if defined(CONFIG_ECU_NVM)
	ret = ecu_nvm_init();
	if (ret != 0) {
		LOG_ERR("Persistent storage initialization failed: %d", ret);
		return ret;
	}
#else
	LOG_WRN("NVM disabled: persistent configuration unavailable on this build");
#endif
	ret = can_if_init();
	if (ret != 0) {
		LOG_ERR("CAN interface initialization failed: %d", ret);
		return ret;
	}
	ret = can_comm_init();
	if (ret == 0) { ret = app_tasks_init(); }
	if (ret != 0) {
		LOG_ERR("Application communication initialization failed: %d", ret);
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
	ret = can_comm_start();
	if (ret == 0) { ret = ecu_task_manager_start(); }
	if (ret != 0) {
		LOG_ERR("Application task startup failed: %d", ret);
		return ret;
	}
	LOG_INF("ZephyrECU CAN and diagnostics ready");
	return 0;
}
