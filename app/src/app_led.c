#include "app_led.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#if !defined(CONFIG_BOARD_NATIVE_SIM)
#include <zephyr/drivers/gpio.h>
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#endif

LOG_MODULE_REGISTER(app_led);
static atomic_t led_on;
static bool initialized;

int app_led_init(void)
{
#if !defined(CONFIG_BOARD_NATIVE_SIM)
	if (!gpio_is_ready_dt(&led)) {
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (ret != 0) {
		return ret;
	}
#endif
	atomic_clear(&led_on);
	initialized = true;
	LOG_INF("LED initialized: off (%s)",
		IS_ENABLED(CONFIG_BOARD_NATIVE_SIM) ? "memory backend" : "DT led0");
	return 0;
}

int app_led_set(bool on)
{
	if (!initialized) {
		return -EACCES;
	}
#if !defined(CONFIG_BOARD_NATIVE_SIM)
	int ret = gpio_pin_set_dt(&led, on);

	if (ret != 0) {
		return ret;
	}
#endif
	atomic_set(&led_on, on);
	LOG_INF("LED %s", on ? "on" : "off");
	return 0;
}

bool app_led_get(void)
{
	return atomic_get(&led_on) != 0;
}
