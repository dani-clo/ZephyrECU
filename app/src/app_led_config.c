#include "app_led_config.h"
#include "app_tasks.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_ECU_NVM)
#include "ecu_nvm.h"
#endif

LOG_MODULE_REGISTER(app_led_config);
static struct k_spinlock config_lock;
static bool ready, enabled = true;
#if defined(CONFIG_ECU_NVM)
static uint64_t generation, saved_generation;
static int write_error;
K_SEM_DEFINE(config_wake, 0, 1);

static void persistence_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (true) {
		k_sem_take(&config_wake, K_FOREVER);
		k_spinlock_key_t key = k_spin_lock(&config_lock);
		uint8_t snapshot = enabled;
		uint64_t snapshot_generation = generation;
		bool dirty = generation != saved_generation;

		k_spin_unlock(&config_lock, key);
		if (!dirty) { continue; }
		/* Synchronous flash only here, never holding the SWC spinlock. */
		int ret = ecu_nvm_write(APP_LED_CONFIG_NVM_ID, APP_LED_CONFIG_VERSION,
					&snapshot, sizeof(snapshot));

		key = k_spin_lock(&config_lock);
		write_error = ret;
		if (ret == 0) { saved_generation = snapshot_generation; }
		bool newer = generation != snapshot_generation;

		k_spin_unlock(&config_lock, key);
		if (ret != 0) { LOG_ERR("LED configuration save failed: %d", ret); }
		/* Preserve updates received during a write, including when that write
		 * failed. Do not endlessly retry an unchanged failed snapshot.
		 */
		if (newer) { k_sem_give(&config_wake); }
	}
}
K_THREAD_DEFINE(app_led_persistence_thread, CONFIG_APP_LED_PERSIST_STACK_SIZE,
	persistence_worker, NULL, NULL, NULL, CONFIG_APP_LED_PERSIST_PRIORITY, 0, SYS_FOREVER_MS);
#endif

int app_led_config_init(void)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	/* Startup-only; application serializes initialization before publication. */
	if (ready) { return -EALREADY; }
	uint8_t value = 1;
#if defined(CONFIG_ECU_NVM)
	int ret = ecu_nvm_read(APP_LED_CONFIG_NVM_ID, APP_LED_CONFIG_VERSION, &value, sizeof(value));

	if (ret != 0 && ret != -ENOENT) { return ret; }
	if (value > 1) { return -EBADMSG; } /* SWC owns its payload's meaning */
#endif
	enabled = value != 0;
	ready = true;
#if defined(CONFIG_ECU_NVM)
	k_thread_start(app_led_persistence_thread);
#endif
	return 0;
}

bool app_led_config_enabled(void)
{
	k_spinlock_key_t key = k_spin_lock(&config_lock);
	bool value = enabled;

	k_spin_unlock(&config_lock, key);
	return value;
}

int app_led_config_set_enabled(bool value)
{
	if (k_is_in_isr()) { return -EWOULDBLOCK; }
	k_spinlock_key_t key = k_spin_lock(&config_lock);

	if (!ready) { k_spin_unlock(&config_lock, key); return -EACCES; }
	if (enabled != value) {
		enabled = value;
#if defined(CONFIG_ECU_NVM)
		++generation;
#endif
	}
	k_spin_unlock(&config_lock, key);
	return app_tasks_config_changed();
}

void app_led_config_request_persistence(void)
{
#if defined(CONFIG_ECU_NVM)
	k_sem_give(&config_wake);
#endif
}

void app_led_config_status(bool *pending, int *last_error)
{
	k_spinlock_key_t key = k_spin_lock(&config_lock);
#if defined(CONFIG_ECU_NVM)
	if (pending != NULL) { *pending = generation != saved_generation; }
	if (last_error != NULL) { *last_error = write_error; }
#else
	if (pending != NULL) { *pending = false; }
	if (last_error != NULL) { *last_error = -ENOTSUP; }
#endif
	k_spin_unlock(&config_lock, key);
}
