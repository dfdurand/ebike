/*
 * eBike tracker — geofence: alert when the bike leaves a radius around a
 * fixed point.
 *
 * Computed on device: a zbus listener on CH_GNSS_FIX compares every valid fix
 * with the home point (equirectangular distance). An exit is declared only
 * when `distance - accuracy > radius` on CONFIG_EBIKE_GEOFENCE_EXIT_COUNT
 * consecutive fixes; a return needs the bike well inside 90% of the radius.
 * Events go out on CH_ALERT, which cloud.c publishes immediately, with a
 * reminder every 60 s while the bike stays outside.
 *
 * Independent of `armed`: the geofence has its own `geofence_enabled` flag.
 * Arming already alerts on any movement, so the geofence is what covers a
 * lent or shared bike, or an owner who forgot to arm.
 *
 * No init function: the listener attaches statically and stays inert until the
 * configuration enables it.
 */

#ifndef EBIKE_GEOFENCE_H_
#define EBIKE_GEOFENCE_H_

#include "events/channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply the geofence configuration (home, radius, enabled).
 *
 * Called by the state machine at boot with the NVS values, and on every
 * CH_CONFIG (Device Shadow or set_home command). It is a no-op when the
 * geofence fields are unchanged; otherwise the zone state restarts from
 * "inside", so the next out-of-zone fix restarts the exit count.
 */
void geofence_apply_config(const struct msg_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_GEOFENCE_H_ */
