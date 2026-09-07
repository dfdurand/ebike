/*
 * eBike tracker — geofence implementation.
 */

#include "geofence.h"
#include "events/channels.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <math.h>

LOG_MODULE_REGISTER(geofence, CONFIG_EBIKE_LOG_LEVEL);

/* New alert at most every N ms while the bike stays out of the zone. */
#define REALERT_MS (60 * MSEC_PER_SEC)

/* A return is declared below this fraction of the radius: spatial hysteresis
 * against OUT/IN flapping when the bike is parked near the border. */
#define REENTRY_NUM 9
#define REENTRY_DEN 10

/* TEMPORAL debounce: minimum delay before declaring a zone state change in the
 * opposite direction of the previous one.
 *
 * Spatial hysteresis alone is not enough when the bike sits right on the
 * boundary: GNSS accuracy (a few metres median, tens of metres at p90) makes
 * the measured distance oscillate across the threshold, which produced four
 * alerts and four e-mails in 82 seconds on a stationary bike.
 *
 * This only delays reversals. A genuine exit is not slowed down, and the 60 s
 * out-of-zone reminders keep running. */
#define FLIP_GUARD_MS (CONFIG_EBIKE_GEOFENCE_FLIP_GUARD_S * MSEC_PER_SEC)

#define EARTH_RADIUS_M 6371000.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG_TO_RAD (M_PI / 180.0)

static struct k_spinlock lock;

/* Configuration and zone state, protected by the spinlock: written by the
 * state machine through geofence_apply_config(), read and written by the
 * listener in workqueue context. */
static struct {
	double home_lat;
	double home_lon;
	uint32_t radius_m;
	bool active;         /* enabled + home defined + radius > 0 */
	bool out;            /* bike currently outside */
	uint8_t exit_streak; /* consecutive fixes seen outside */
	int64_t last_alert_ms;
	int64_t last_flip_ms; /* last zone state CHANGE (temporal debounce) */
} gf;

/* Is a zone state change allowed right now? */
static bool flip_allowed(int64_t now)
{
	if (FLIP_GUARD_MS == 0 || gf.last_flip_ms == 0) {
		return true;
	}
	return (now - gf.last_flip_ms) >= FLIP_GUARD_MS;
}

/* Equirectangular distance (m): the error is negligible at geofence scale
 * (below a few tens of km) and it is cheaper than haversine. */
static double distance_to_home_m(double home_lat, double home_lon,
				 double lat, double lon)
{
	double x = (lon - home_lon) * DEG_TO_RAD *
		   cos(((lat + home_lat) / 2.0) * DEG_TO_RAD);
	double y = (lat - home_lat) * DEG_TO_RAD;

	return EARTH_RADIUS_M * sqrt(x * x + y * y);
}

void geofence_apply_config(const struct msg_config *cfg)
{
	bool active = cfg->geofence_enabled && cfg->home_set &&
		      cfg->geofence_radius_m > 0;
	bool changed = false;

	K_SPINLOCK(&lock) {
		changed = active != gf.active ||
			  cfg->home_lat != gf.home_lat ||
			  cfg->home_lon != gf.home_lon ||
			  cfg->geofence_radius_m != gf.radius_m;

		if (changed) {
			gf.home_lat = cfg->home_lat;
			gf.home_lon = cfg->home_lon;
			gf.radius_m = cfg->geofence_radius_m;
			gf.active = active;
			/* Clean restart: the next out-of-zone fix restarts the
			 * count, and the alert with it. */
			gf.out = false;
			gf.exit_streak = 0;
			gf.last_flip_ms = 0; /* no inherited guard time */
		}
	}

	if (!changed) {
		return;
	}

	if (active) {
		LOG_INF("Geofence active: home %.6f, %.6f — radius %u m",
			cfg->home_lat, cfg->home_lon, cfg->geofence_radius_m);
	} else if (cfg->geofence_enabled && !cfg->home_set) {
		LOG_WRN("Geofence requested but no home defined — inactive "
			"(set shadow home_lat/home_lon or send "
			"{\"set_home\":true})");
	} else {
		LOG_INF("Geofence inactive");
	}
}

static void gnss_fix_listener_cb(const struct zbus_channel *chan)
{
	const struct msg_gnss_fix *fix = zbus_chan_const_msg(chan);
	struct msg_alert alert = { .cause = ALERT_NONE };
	int64_t now = k_uptime_get();
	double home_lat, home_lon;
	uint32_t radius_m;
	bool active;

	if (!fix->valid) {
		return;
	}

	K_SPINLOCK(&lock) {
		active = gf.active;
		home_lat = gf.home_lat;
		home_lon = gf.home_lon;
		radius_m = gf.radius_m;
	}

	if (!active) {
		return;
	}

	double dist = distance_to_home_m(home_lat, home_lon,
					 fix->lat, fix->lon);
	/* Exit: certain even at the worst of the GNSS uncertainty. */
	bool exit_fix = (dist - (double)fix->acc_m) > (double)radius_m;
	/* Return: clearly inside (hysteresis). */
	bool inside = dist < (double)radius_m * REENTRY_NUM / REENTRY_DEN;

	K_SPINLOCK(&lock) {
		if (!gf.active) {
			/* Configuration removed between the two sections. */
		} else if (!gf.out) {
			if (exit_fix) {
				gf.exit_streak++;
				if (gf.exit_streak >=
					    CONFIG_EBIKE_GEOFENCE_EXIT_COUNT &&
				    flip_allowed(now)) {
					gf.out = true;
					gf.last_alert_ms = now;
					gf.last_flip_ms = now;
					alert.cause = ALERT_GEOFENCE_EXIT;
				}
			} else {
				gf.exit_streak = 0;
			}
		} else if (inside) {
			/* Back inside. While the guard time still runs, declare
			 * NOTHING — and above all not an exit reminder, which
			 * would announce a bike that has returned as still
			 * being away. */
			if (flip_allowed(now)) {
				gf.out = false;
				gf.exit_streak = 0;
				gf.last_flip_ms = now;
				alert.cause = ALERT_GEOFENCE_RETURN;
			}
		} else if (now - gf.last_alert_ms >= REALERT_MS) {
			gf.last_alert_ms = now;
			alert.cause = ALERT_GEOFENCE_EXIT;
		}
	}

	if (alert.cause == ALERT_NONE) {
		return;
	}

	alert.dist_m = (uint32_t)dist;
	alert.has_pos = true;
	alert.lat = fix->lat;
	alert.lon = fix->lon;
	alert.acc_m = fix->acc_m;
	alert.fix_age_s = 0; /* THIS fix is the trigger, so it is fresh */

	if (alert.cause == ALERT_GEOFENCE_EXIT) {
		LOG_WRN("Out of zone: %u m from home (radius %u m)",
			alert.dist_m, radius_m);
	} else {
		LOG_INF("Back in zone (%u m from home)", alert.dist_m);
	}

	/* The cloud listener publishes on bike/<id>/alert from this very
	 * context, so the alert leaves immediately, outside the telemetry
	 * cycle. */
	(void)zbus_chan_pub(&CH_ALERT, &alert, K_MSEC(100));
}

ZBUS_LISTENER_DEFINE(geofence_fix_lis, gnss_fix_listener_cb);
ZBUS_CHAN_ADD_OBS(CH_GNSS_FIX, geofence_fix_lis, 3);
