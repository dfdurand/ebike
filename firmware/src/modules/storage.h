/*
 * eBike tracker — persistent configuration in NVS.
 *
 * Backed by the `storage` partition of the bootloader-less flash layout.
 * Holds the wheel circumference, the anti-theft configuration pushed by the
 * Device Shadow (armed, publish interval, motion threshold) and the geofence
 * (home point, radius, enabled). Every getter falls back to its Kconfig
 * default when NVS is empty or unavailable.
 */

#ifndef EBIKE_STORAGE_H_
#define EBIKE_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount NVS on the `storage` partition.
 *
 * @retval 0 on success, negative errno otherwise — the application can carry
 *         on with the Kconfig defaults.
 */
int storage_init(void);

/** Wheel circumference (mm), CONFIG_EBIKE_WHEEL_CIRC_MM when unset. */
uint32_t storage_get_wheel_circ_mm(void);
int storage_set_wheel_circ_mm(uint32_t circ_mm);

/** Anti-theft armed state. Survives reboots, including a battery cut. */
bool storage_get_armed(void);
int storage_set_armed(bool armed);

/** RIDING/ALARM publish interval (s), CONFIG_EBIKE_PUBLISH_RIDING_S default. */
uint32_t storage_get_publish_interval_s(void);
int storage_set_publish_interval_s(uint32_t seconds);

/** IMU motion threshold (mg), CONFIG_EBIKE_MOTION_THRESHOLD_MG default. */
uint32_t storage_get_motion_threshold_mg(void);
int storage_set_motion_threshold_mg(uint32_t mg);

/**
 * @brief Geofence reference point.
 *
 * @retval true when a home has been persisted (lat/lon filled in); the
 *         geofence stays inactive until one is defined.
 */
bool storage_get_home(double *lat, double *lon);
int storage_set_home(double lat, double lon);

/** Geofence radius (m), CONFIG_EBIKE_GEOFENCE_RADIUS_M default. */
uint32_t storage_get_geofence_radius_m(void);
int storage_set_geofence_radius_m(uint32_t radius_m);

/** Geofence enable flag. Survives reboots, like the armed state. */
bool storage_get_geofence_enabled(void);
int storage_set_geofence_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_STORAGE_H_ */
