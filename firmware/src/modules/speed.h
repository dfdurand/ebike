/*
 * eBike tracker — wheel speed from a reed/Hall sensor (one magnet per turn).
 *
 * One pulse per wheel turn on the `speed_sensor` GPIO. The ISR only does
 * debouncing, timestamping and counting; speed is computed on read as
 * circumference / dt, and falls back to zero when no pulse has arrived for
 * CONFIG_EBIKE_SPEED_STOP_TIMEOUT_S.
 */

#ifndef EBIKE_SPEED_H_
#define EBIKE_SPEED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the sensor GPIO and arm its interrupt.
 *
 * Call after storage_init() so the persisted wheel circumference is applied.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int speed_init(void);

/**
 * @brief Instantaneous wheel speed (km/h).
 *
 * Returns 0.0 when the wheel is stopped (no pulse for
 * CONFIG_EBIKE_SPEED_STOP_TIMEOUT_S) or nothing has been measured yet.
 */
float speed_get_kmh(void);

/** Total wheel turns since boot (simple odometer). */
uint32_t speed_rev_count(void);

/**
 * @brief Change the wheel circumference used in the computation (mm).
 *
 * Takes effect immediately; persisting it is the caller's job.
 */
void speed_set_wheel_circ_mm(uint32_t circ_mm);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_SPEED_H_ */
