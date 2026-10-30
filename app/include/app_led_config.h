#ifndef ZEPHYRECU_APP_LED_CONFIG_H_
#define ZEPHYRECU_APP_LED_CONFIG_H_

#include <stdbool.h>

/* LED SWC owns fixed SWC ID 0x1000, payload version 1, one byte (0/1). */
#define APP_LED_CONFIG_NVM_ID 0x1000U
#define APP_LED_CONFIG_VERSION 1U

/* After ecu_nvm_init (when enabled), before application Task Manager start.
 * Missing record -> enabled default. All other read/format errors fail startup.
 */
int app_led_config_init(void);
bool app_led_config_enabled(void);
/* Thread-only, asynchronous SWC-level acceptance. Updates latest synchronized
 * state and notifies its application task. A repeated value retries dirty data.
 * Use status to observe durability; no flash operations in this call.
 */
int app_led_config_set_enabled(bool enabled);
/* Task callback: notify the SWC-owned persistence thread without waiting. */
void app_led_config_request_persistence(void);
/* Snapshot: pending includes unsaved changes; last_error is the last completed
 * write outcome. Failure remains visible/dirty until explicit retry/change.
 */
void app_led_config_status(bool *pending, int *last_error);
#endif
