/*
 * eBike tracker — IMU module: LSM6DS0 accelerometer (X-NUCLEO-IKS01A1 shield).
 *
 * The Zephyr lsm6ds0 driver has no trigger support (no data-ready or wake-up
 * interrupt), so the accelerometer is polled: a work item samples it every
 * CONFIG_EBIKE_IMU_POLL_MS, caches the reading and publishes CH_MOTION.
 *
 * The imu_ prefix avoids colliding with Zephyr's Sensor API namespace.
 */

#ifndef EBIKE_SENSOR_H_
#define EBIKE_SENSOR_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Latest acceleration reading, in the sensor frame. */
struct imu_accel {
	float x_ms2;
	float y_ms2;
	float z_ms2;
	int64_t age_ms; /* age of the reading, computed on read */
};

/**
 * @brief Check the IMU and start periodic sampling.
 *
 * @retval 0 on success, negative errno when the sensor is missing or silent
 *         (shield not fitted) — the application can run without it.
 */
int imu_init(void);

/**
 * @brief Fetch the latest acceleration reading.
 *
 * @param[out] out Filled when a valid reading exists.
 * @retval true when a valid reading is available.
 */
bool imu_get_accel(struct imu_accel *out);

/**
 * @brief Change the motion detection threshold (milli-g).
 *
 * Pushed by the Device Shadow; defaults to CONFIG_EBIKE_MOTION_THRESHOLD_MG.
 * Persisting the value is the caller's job.
 */
void imu_set_motion_threshold_mg(uint32_t mg);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_SENSOR_H_ */
