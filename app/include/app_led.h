#ifndef ZEPHYRECU_APP_LED_H_
#define ZEPHYRECU_APP_LED_H_

#include <stdbool.h>

/* Initialize the demo LED in its safe (off) state. */
int app_led_init(void);
int app_led_set(bool on);
bool app_led_get(void);

/* Separate RGB channel reserved for the benign diagnostic routine. */
int app_led_diag_set(bool on);
bool app_led_diag_get(void);

#endif
