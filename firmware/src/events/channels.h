/*
 * eBike tracker — zbus channels, the event backbone of the firmware.
 *
 * Design rules:
 *  - Each channel has ONE producer that publishes its state at its own
 *    natural rate. zbus keeps the last message, so snapshot consumers (the
 *    telemetry builder in cloud.c) just read the channel when they need it,
 *    without any coupling between modules.
 *  - Reactive consumers (state machine, alert publisher) subscribe and are
 *    only woken by publications; they filter what matters to them.
 *  - Bulk data stays OFF the bus: the batched GNSS trace uses a ring buffer
 *    (location_drain_samples), because a channel carries one message, not a
 *    queue of points.
 *
 * Producer -> channel:
 *   connectivity  -> CH_LTE_STATUS  (network registration changes)
 *   speed         -> CH_SPEED       (1 s ticker: wheel speed + odometer)
 *   sensor (IMU)  -> CH_MOTION      (1 s poll: acceleration + motion)
 *   location      -> CH_GNSS_FIX    (every valid fix, up to ~1 Hz)
 *   cloud         -> CH_CLOUD_CMD   (message received on bike/<id>/cmd)
 *   cloud         -> CH_CONFIG      (Device Shadow: armed + configuration)
 *   state machine -> CH_ALERT       (entering the ALARM state)
 *   geofence      -> CH_ALERT       (area exit / return)
 */

#ifndef EBIKE_CHANNELS_H_
#define EBIKE_CHANNELS_H_

#include <zephyr/zbus/zbus.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CH_LTE_STATUS — published by connectivity on every state change. */
struct msg_lte_status {
	bool connected; /* registered, home or roaming */
};

/** CH_SPEED — published by speed every second. */
struct msg_speed {
	float kmh;     /* instantaneous wheel speed (0 when stopped) */
	uint32_t revs; /* odometer, wheel turns since boot */
	bool moving;   /* wheel turning (kmh > 0) */
};

/** CH_MOTION — published by sensor (IMU) on every poll. */
struct msg_motion {
	float acc_ms2[3]; /* latest acceleration (m/s2) */
	float delta_ms2;  /* |delta| since the previous poll (m/s2) */
	bool moving;      /* delta above CONFIG_EBIKE_MOTION_THRESHOLD_MG */
};

/** CH_GNSS_FIX — published by location on every valid fix. */
struct msg_gnss_fix {
	double lat;
	double lon;
	float acc_m;
	float spd_kmh;
	uint8_t sats;
	bool valid; /* false until the first fix since boot */
};

/** CH_CLOUD_CMD — published by cloud when a cmd message arrives. */
struct msg_cloud_cmd {
	char json[96]; /* raw payload, truncated if longer (NUL terminated) */
};

/**
 * CH_CONFIG — published by cloud (Device Shadow desired/delta) and by main at
 * boot (NVS values). It carries the merged view: cloud reads the channel,
 * applies the fields present in the shadow and republishes. The state machine
 * applies and persists them.
 */
struct msg_config {
	bool armed;                  /* anti-theft armed */
	uint32_t publish_interval_s; /* RIDING/ALARM publish rate */
	uint32_t motion_threshold_mg;

	/* On-device geofence. */
	double home_lat;            /* reference point */
	double home_lon;
	bool home_set;              /* false until a home is defined */
	uint32_t geofence_radius_m; /* allowed radius around home */
	bool geofence_enabled;
};

/** CH_ALERT — published by the state machine (ALARM) and by geofence. */
enum alert_cause {
	ALERT_NONE = 0,
	ALERT_MOTION_WHILE_ARMED, /* 1: movement while armed */
	ALERT_GEOFENCE_EXIT,      /* 2: left the allowed radius */
	ALERT_GEOFENCE_RETURN,    /* 3: back inside the area */
};

struct msg_alert {
	enum alert_cause cause;

	/* Position attached to the alert. Geofence causes always carry one (a
	 * fix is what triggers them); the theft cause takes the last known fix,
	 * hence fix_age_s: a position several minutes old does not say where
	 * the bike is, and the recipient must be able to tell.
	 * has_pos is false when no fix is available at all. */
	bool has_pos;
	double lat;
	double lon;
	float acc_m;       /* horizontal uncertainty of the fix (m) */
	int32_t fix_age_s; /* age of the fix (s), -1 when unknown */

	uint32_t dist_m; /* distance to home — geofence causes only */
};

ZBUS_CHAN_DECLARE(CH_LTE_STATUS);
ZBUS_CHAN_DECLARE(CH_SPEED);
ZBUS_CHAN_DECLARE(CH_MOTION);
ZBUS_CHAN_DECLARE(CH_GNSS_FIX);
ZBUS_CHAN_DECLARE(CH_CLOUD_CMD);
ZBUS_CHAN_DECLARE(CH_CONFIG);
ZBUS_CHAN_DECLARE(CH_ALERT);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_CHANNELS_H_ */
