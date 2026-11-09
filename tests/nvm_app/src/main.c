#include "ecu_nvm.h"
#include "ecu_task_manager.h"
#include "app_led.h"
#include "app_tasks.h"
#include "app_led_config.h"
#include "can_comm.h"
#include "can_if.h"

#include <errno.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

DEFINE_FFF_GLOBALS;
extern const k_tid_t app_led_persistence_thread;
ECU_TASK_DECLARE(app_monitor_task);
static const struct device *const flash = PARTITION_NODE_DEVICE(DT_ALIAS(ecu_nvm));
K_SEM_DEFINE(write_entered, 0, 1);
K_SEM_DEFINE(write_continue, 0, 1);
static atomic_t hold_write, fail_write, write_bytes;

static int write_hook(const struct device *dev, off_t offset, uint8_t data)
{
	ARG_UNUSED(dev); ARG_UNUSED(offset);
	zassert_equal(k_current_get(), app_led_persistence_thread);
	atomic_inc(&write_bytes);
	if (atomic_cas(&hold_write, 1, 0)) {
		k_sem_give(&write_entered);
		zassert_ok(k_sem_take(&write_continue, K_SECONDS(2)));
	}
	return atomic_get(&fail_write) ? -EIO : data;
}
static const struct flash_simulator_cb hooks = {.write_byte = write_hook};

static void wait_saved(void)
{
	bool pending = true;
	int error;
	int64_t until = k_uptime_get() + 1000;

	do {
		k_sleep(K_MSEC(1));
		app_led_config_status(&pending, &error);
	} while (pending && k_uptime_get() < until);
	zassert_false(pending);
	zassert_ok(error);
}

ZTEST(nvm_app, test_ownership_and_updates)
{
	zassert_ok(flash_erase(flash, DT_REG_ADDR(DT_ALIAS(ecu_nvm)),
			       DT_REG_SIZE(DT_ALIAS(ecu_nvm))));
	zassert_ok(app_led_init());
	zassert_ok(can_if_init());
	zassert_ok(can_comm_init());
	zassert_equal(app_tasks_init(), -EACCES); /* config requires initialized NVM */
	zassert_ok(ecu_nvm_init());
	uint8_t bad = 1;

	zassert_ok(ecu_nvm_write(APP_LED_CONFIG_NVM_ID, 2, &bad, 1));
	zassert_equal(app_tasks_init(), -EPROTONOSUPPORT); /* no silent fallback */
	zassert_ok(ecu_nvm_read(APP_LED_CONFIG_NVM_ID, 2, &bad, 1)); /* not overwritten */
	zassert_ok(ecu_nvm_delete(APP_LED_CONFIG_NVM_ID));
	bad = 2;
	zassert_ok(ecu_nvm_write(APP_LED_CONFIG_NVM_ID, APP_LED_CONFIG_VERSION, &bad, 1));
	zassert_equal(app_tasks_init(), -EBADMSG); /* SWC semantic validation */
	zassert_ok(ecu_nvm_delete(APP_LED_CONFIG_NVM_ID));
	zassert_ok(app_tasks_init());
	zassert_true(app_led_config_enabled()); /* absent -> explicit default */
	flash_simulator_set_callbacks(flash, &hooks);

	/* Change accepted before manager start; neither callbacks nor flash run. */
	atomic_set(&hold_write, 1);
	zassert_ok(app_led_config_set_enabled(false));
	k_sleep(K_MSEC(20));
	zassert_equal(atomic_get(&write_bytes), 0);
	zassert_ok(ecu_task_manager_start());
	zassert_ok(k_sem_take(&write_entered, K_SECONDS(1)));
	/* First write is held inside the simulator. Update while it is in flight. */
	ecu_task_stats_t before, after;

	zassert_ok(ecu_task_get_stats(app_monitor_task, &before));
	zassert_ok(app_led_config_set_enabled(true));
	k_sleep(K_MSEC(20)); /* manager continues handling events/periods */
	zassert_ok(ecu_task_get_stats(app_monitor_task, &after));
	zassert_true(after.invocations > before.invocations);
	k_sem_give(&write_continue);
	wait_saved();
	uint8_t persisted;

	zassert_ok(ecu_nvm_read(APP_LED_CONFIG_NVM_ID, APP_LED_CONFIG_VERSION, &persisted, 1));
	zassert_equal(persisted, 1);
	int count = atomic_get(&write_bytes);

	zassert_ok(app_led_config_set_enabled(true));
	k_sleep(K_MSEC(30));
	zassert_equal(atomic_get(&write_bytes), count); /* clean, unchanged: no flash */

	atomic_set(&fail_write, 1);
	zassert_ok(app_led_config_set_enabled(false));
	bool pending;
	int error;
	int64_t until = k_uptime_get() + 1000;

	do {
		k_sleep(K_MSEC(1));
		app_led_config_status(&pending, &error);
	} while (error == 0 && k_uptime_get() < until);
	zassert_equal(error, -EIO);
	zassert_true(pending); /* visible error; change remains dirty, not lost */
	atomic_clear(&fail_write);
	zassert_ok(app_led_config_set_enabled(false)); /* explicit retry, same value */
	wait_saved();
	zassert_ok(ecu_nvm_read(APP_LED_CONFIG_NVM_ID, APP_LED_CONFIG_VERSION, &persisted, 1));
	zassert_equal(persisted, 0);
	flash_simulator_set_callbacks(flash, NULL);
}
ZTEST_SUITE(nvm_app, NULL, NULL, NULL, NULL, NULL);
