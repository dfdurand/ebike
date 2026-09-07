/*
 * eBike tracker — IMU module implementation (LSM6DS0 polling).
 */

#include "sensor.h"
#include "events/channels.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <math.h>

LOG_MODULE_REGISTER(imu, CONFIG_EBIKE_LOG_LEVEL);

static const struct device *const accel_dev =
	DEVICE_DT_GET(DT_ALIAS(accel0));

static struct k_spinlock accel_lock;
static struct imu_accel last_accel;
static int64_t last_accel_uptime = -1;

/* Consecutive read errors: log the first one, then stay quiet so a shield
 * unplugged at runtime does not flood the console. */
static uint32_t fetch_err_streak;

/* Motion detection threshold (mg), adjustable through the Device Shadow. */
static uint32_t motion_threshold_mg = CONFIG_EBIKE_MOTION_THRESHOLD_MG;

static void poll_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_fn);

static void poll_work_fn(struct k_work *work)
{
	struct sensor_value val[3];
	int err;

	err = sensor_sample_fetch_chan(accel_dev, SENSOR_CHAN_ACCEL_XYZ);
	if (!err) {
		err = sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_XYZ, val);
	}

	if (err) {
		if (fetch_err_streak++ == 0) {
			LOG_ERR("Accelerometer read failed (%d)", err);
		}
	} else {
		if (fetch_err_streak > 1) {
			LOG_WRN("Accelerometer back after %u failures",
				fetch_err_streak);
		}
		fetch_err_streak = 0;

		struct msg_motion msg;

		K_SPINLOCK(&accel_lock) {
			last_accel.x_ms2 = (float)sensor_value_to_double(&val[0]);
			last_accel.y_ms2 = (float)sensor_value_to_double(&val[1]);
			last_accel.z_ms2 = (float)sensor_value_to_double(&val[2]);
			last_accel_uptime = k_uptime_get();

			msg.acc_ms2[0] = last_accel.x_ms2;
			msg.acc_ms2[1] = last_accel.y_ms2;
			msg.acc_ms2[2] = last_accel.z_ms2;
		}

		/* Motion detection uses the norm of the acceleration delta
		 * between two polls. Gravity cancels out in the difference, so
		 * the test does not care how the bike is oriented. */
		static float prev[3];
		static bool prev_valid;
		const float thr_ms2 =
			(float)motion_threshold_mg * 9.80665f / 1000.0f;

		if (prev_valid) {
			const float dx = msg.acc_ms2[0] - prev[0];
			const float dy = msg.acc_ms2[1] - prev[1];
			const float dz = msg.acc_ms2[2] - prev[2];

			msg.delta_ms2 = sqrtf(dx * dx + dy * dy + dz * dz);
			msg.moving = (msg.delta_ms2 > thr_ms2);
		} else {
			msg.delta_ms2 = 0.0f;
			msg.moving = false;
		}

		prev[0] = msg.acc_ms2[0];
		prev[1] = msg.acc_ms2[1];
		prev[2] = msg.acc_ms2[2];
		prev_valid = true;

		(void)zbus_chan_pub(&CH_MOTION, &msg, K_MSEC(100));
	}

	(void)k_work_schedule(&poll_work, K_MSEC(CONFIG_EBIKE_IMU_POLL_MS));
}

int imu_init(void)
{
	if (!device_is_ready(accel_dev)) {
		LOG_ERR("IMU %s not ready (X-NUCLEO-IKS01A1 shield fitted?)",
			accel_dev->name);
		return -ENODEV;
	}

	LOG_INF("IMU %s ready — polling accelerometer every %d ms",
		accel_dev->name, CONFIG_EBIKE_IMU_POLL_MS);

	(void)k_work_schedule(&poll_work, K_NO_WAIT);

	return 0;
}

void imu_set_motion_threshold_mg(uint32_t mg)
{
	if (mg == 0) {
		return;
	}
	motion_threshold_mg = mg;
	LOG_INF("IMU motion threshold: %u mg", mg);
}

bool imu_get_accel(struct imu_accel *out)
{
	bool valid = false;

	K_SPINLOCK(&accel_lock) {
		if (last_accel_uptime >= 0) {
			*out = last_accel;
			out->age_ms = k_uptime_get() - last_accel_uptime;
			valid = true;
		}
	}

	return valid;
}
