/*
 * eBike tracker — zbus channel definitions.
 *
 * Observers are deliberately NOT listed here: each consumer attaches from its
 * own file with ZBUS_CHAN_ADD_OBS (the state machine in main.c, the alert
 * listener in cloud.c), which keeps this file free of module dependencies.
 */

#include "channels.h"

ZBUS_CHAN_DEFINE(CH_LTE_STATUS,
		 struct msg_lte_status,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.connected = false));

ZBUS_CHAN_DEFINE(CH_SPEED,
		 struct msg_speed,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.kmh = 0.0f, .revs = 0, .moving = false));

ZBUS_CHAN_DEFINE(CH_MOTION,
		 struct msg_motion,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.moving = false));

ZBUS_CHAN_DEFINE(CH_GNSS_FIX,
		 struct msg_gnss_fix,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.valid = false));

ZBUS_CHAN_DEFINE(CH_CLOUD_CMD,
		 struct msg_cloud_cmd,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.json = ""));

ZBUS_CHAN_DEFINE(CH_CONFIG,
		 struct msg_config,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.armed = false,
			       .publish_interval_s = CONFIG_EBIKE_PUBLISH_RIDING_S,
			       .motion_threshold_mg = CONFIG_EBIKE_MOTION_THRESHOLD_MG,
			       .home_set = false,
			       .geofence_radius_m = CONFIG_EBIKE_GEOFENCE_RADIUS_M,
			       .geofence_enabled = false));

ZBUS_CHAN_DEFINE(CH_ALERT,
		 struct msg_alert,
		 NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.cause = ALERT_NONE));
