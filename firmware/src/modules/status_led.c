/*
 * eBike tracker — status LEDs implementation.
 */

#include "status_led.h"

#if defined(CONFIG_EBIKE_STATUS_LED)

#include "cloud.h"
#include "connectivity.h"
#include "location.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_led, CONFIG_EBIKE_LOG_LEVEL);

/* Blink patterns: one bit per tick, replayed in a loop. SLOT_COUNT ticks make
 * one cycle, so with the default 100 ms tick a cycle lasts 1 s. Encoding the
 * patterns as bitmaps rather than counting elapsed time keeps every LED in
 * step and makes a new pattern a one-line change. */
#define SLOT_COUNT 10

#define PAT_DARK  0x000U /* off */
#define PAT_BLIP  0x001U /* one slot on: alive, nothing more to say */
#define PAT_BLINK 0x01FU /* half on, half off: an even 1 Hz blink */
#define PAT_SOLID 0x3FFU /* always on */

static const struct gpio_dt_spec led_alive =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_link =
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

#if defined(CONFIG_EBIKE_STATUS_LED_EXTENDED)
static const struct gpio_dt_spec led_cloud =
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec led_gnss =
	GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
#endif

static const struct gpio_dt_spec *const leds[] = {
	&led_alive,
	&led_link,
#if defined(CONFIG_EBIKE_STATUS_LED_EXTENDED)
	&led_cloud,
	&led_gnss,
#endif
};

static void tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick, tick_fn);

/* Polling rather than zbus listeners: the states shown here are already
 * exposed as plain getters reading a static, and all of them are safe to call
 * before their own module is initialised (they simply report "not yet"). That
 * keeps this module free of any init-order constraint — it can therefore start
 * first, which is the whole point. */
static void tick_fn(struct k_work *work)
{
	static uint8_t slot;

	const int32_t fix_age = location_fix_age_s();
	const bool fix_fresh = (fix_age >= 0 &&
				fix_age <= CONFIG_EBIKE_STATUS_LED_FIX_FRESH_S);
	uint32_t pat_alive;
	uint32_t pat_link;

	/* LED1 keeps blinking in both cases: whatever the GNSS is doing, the
	 * board must never look dead while it is running. */
	pat_alive = fix_fresh ? PAT_BLINK : PAT_BLIP;

	if (!connectivity_is_connected()) {
		pat_link = PAT_DARK;
	} else if (!cloud_is_connected()) {
		pat_link = PAT_BLINK;
	} else {
		pat_link = PAT_SOLID;
	}

	(void)gpio_pin_set_dt(&led_alive, (pat_alive >> slot) & 1U);
	(void)gpio_pin_set_dt(&led_link, (pat_link >> slot) & 1U);

#if defined(CONFIG_EBIKE_STATUS_LED_EXTENDED)
	(void)gpio_pin_set_dt(&led_cloud, cloud_is_connected() ? 1 : 0);
	(void)gpio_pin_set_dt(&led_gnss, fix_fresh ? 1 : 0);
#endif

	slot = (slot + 1U) % SLOT_COUNT;

	(void)k_work_schedule(&tick, K_MSEC(CONFIG_EBIKE_STATUS_LED_TICK_MS));
}

int status_led_init(void)
{
	for (int i = 0; i < (int)ARRAY_SIZE(leds); i++) {
		int err;

		if (!gpio_is_ready_dt(leds[i])) {
			LOG_WRN("LED%d not ready — status LEDs off", i + 1);
			return -ENODEV;
		}

		err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_WRN("LED%d configuration failed (%d) — status LEDs "
				"off", i + 1, err);
			return err;
		}
	}

	LOG_INF("Status LEDs: 1 = alive (blip) / GNSS fix (blink), "
		"2 = LTE (blink) / cloud (solid)");

	/* Lamp test: every driven LED is lit together for a moment at boot.
	 *
	 * Two things it settles at a glance, and neither is cosmetic. Reaching
	 * this point proves main() ran — on battery it is the earliest possible
	 * sign of life, before the modem init. And it tells a dead indicator
	 * from a false one: the LEDs sit on Arduino header pins, so a mounted
	 * shield can hold one down and keep it dark forever. A LED that misses
	 * the lamp test is physically unusable, which without this looks
	 * exactly like "the cloud never connected". */
	for (int i = 0; i < (int)ARRAY_SIZE(leds); i++) {
		(void)gpio_pin_set_dt(leds[i], 1);
	}

	(void)k_work_schedule(&tick,
			      K_MSEC(CONFIG_EBIKE_STATUS_LED_LAMP_TEST_MS));

	return 0;
}

#else /* CONFIG_EBIKE_STATUS_LED */

int status_led_init(void)
{
	return 0;
}

#endif /* CONFIG_EBIKE_STATUS_LED */
