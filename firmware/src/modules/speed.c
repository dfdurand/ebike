/*
 * eBike tracker — wheel speed module implementation.
 */

#include "speed.h"
#include "events/channels.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(speed, CONFIG_EBIKE_LOG_LEVEL);

static const struct gpio_dt_spec sensor_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(speed_sensor), gpios);

static struct gpio_callback sensor_cb;

/* State shared between the ISR and the threads. The 32-bit fields are read
 * and written atomically on Cortex-M; the 64-bit timestamp is read under
 * irq_lock. */
static volatile int64_t last_edge_ms = -1;
static volatile uint32_t period_ms;
static volatile uint32_t rev_count;

static uint32_t wheel_circ_mm = CONFIG_EBIKE_WHEEL_CIRC_MM;

/* One-second ticker publishing CH_SPEED. The ticker, not the ISR, owns the
 * channel: a STOPPED wheel produces no interrupt at all, so an active tick is
 * what makes "moving = false" reach the state machine and let it park. */
static void speed_tick_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(speed_tick, speed_tick_fn);

static void speed_tick_fn(struct k_work *work)
{
	const float kmh = speed_get_kmh();
	const struct msg_speed msg = {
		.kmh = kmh,
		.revs = rev_count,
		.moving = (kmh > 0.0f),
	};

	(void)zbus_chan_pub(&CH_SPEED, &msg, K_MSEC(100));
	(void)k_work_schedule(&speed_tick, K_SECONDS(1));
}

static void sensor_isr(const struct device *port, struct gpio_callback *cb,
		       uint32_t pins)
{
	int64_t now = k_uptime_get();
	int64_t prev = last_edge_ms;

	/* Debounce: reed contact bounce lasts a couple of milliseconds. */
	if (prev >= 0 && (now - prev) < CONFIG_EBIKE_SPEED_DEBOUNCE_MS) {
		return;
	}

	if (prev >= 0) {
		period_ms = (uint32_t)(now - prev);
	}
	last_edge_ms = now;
	rev_count++;
}

int speed_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&sensor_gpio)) {
		LOG_ERR("Speed sensor GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&sensor_gpio, GPIO_INPUT);
	if (err) {
		LOG_ERR("Speed sensor GPIO configuration failed (%d)", err);
		return err;
	}

	/* Edge to active = magnet arriving in front of the sensor. Only that
	 * edge counts; the release edge is ignored. */
	err = gpio_pin_interrupt_configure_dt(&sensor_gpio,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Speed sensor IRQ configuration failed (%d)", err);
		return err;
	}

	gpio_init_callback(&sensor_cb, sensor_isr, BIT(sensor_gpio.pin));
	err = gpio_add_callback(sensor_gpio.port, &sensor_cb);
	if (err) {
		LOG_ERR("Adding speed sensor callback failed (%d)", err);
		return err;
	}

	LOG_INF("Speed sensor ready (P0.%02u, circumference %u mm, "
		"debounce %d ms, stop after %d s)",
		sensor_gpio.pin, wheel_circ_mm,
		CONFIG_EBIKE_SPEED_DEBOUNCE_MS,
		CONFIG_EBIKE_SPEED_STOP_TIMEOUT_S);

	(void)k_work_schedule(&speed_tick, K_SECONDS(1));

	return 0;
}

float speed_get_kmh(void)
{
	unsigned int key = irq_lock();
	int64_t edge = last_edge_ms;
	uint32_t period = period_ms;

	irq_unlock(key);

	if (edge < 0 || period == 0) {
		return 0.0f;
	}

	int64_t age = k_uptime_get() - edge;

	/* Wheel stopped: no pulse since the timeout. */
	if (age > CONFIG_EBIKE_SPEED_STOP_TIMEOUT_S * MSEC_PER_SEC) {
		return 0.0f;
	}

	/* v [km/h] = (circ [mm] / dt [ms]) * 3.6, with dt = max(period, age).
	 * Taking the age into account means the wheel cannot be reported
	 * faster than circ/age while the next pulse is still pending, so speed
	 * decays smoothly during deceleration instead of holding a stale
	 * value. */
	uint32_t dt_ms = MAX(period, (uint32_t)age);

	return 3.6f * (float)wheel_circ_mm / (float)dt_ms;
}

uint32_t speed_rev_count(void)
{
	return rev_count;
}

void speed_set_wheel_circ_mm(uint32_t circ_mm)
{
	if (circ_mm == 0) {
		return;
	}
	wheel_circ_mm = circ_mm;
	LOG_INF("Wheel circumference: %u mm", circ_mm);
}
