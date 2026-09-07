/*
 * eBike tracker — nRF9161.
 *
 * Boot is sequential and timing sensitive (credential provisioning needs the
 * CFUN=0 window): modem init -> AWS credentials -> LTE attach -> cloud ->
 * GNSS -> sensors. That part stays in main(); the state machine then takes
 * over the running behaviour:
 *
 *   CONNECT --(cloud connected)--> PARKED <=> RIDING
 *                                     | (armed + movement)
 *                                     v
 *                                   ALARM --(disarmed)--> PARKED
 *
 *   RIDING: movement detected — publishing at the configured rate.
 *   PARKED: still for CONFIG_EBIKE_PARK_TIMEOUT_S — reduced rate, which means
 *           fewer LTE wake-ups, more RF windows for GNSS and less cellular
 *           data.
 *   ALARM:  anti-theft, armed through the Device Shadow. Any movement raises
 *           CH_ALERT (immediate MQTT) and tracking returns to the riding rate.
 *           Only a remote disarm leaves this state.
 *
 * The state machine is driven by the zbus channels through a subscriber: the
 * main thread sleeps until something is published (the sensor tickers wake it
 * about once a second, with a 5 s fallback timeout).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

#include <stdio.h>

#include <cJSON.h>

#include "events/channels.h"
#include "modules/connectivity.h"
#include "modules/cloud.h"
#include "modules/diag.h"
#include "modules/geofence.h"
#include "modules/location.h"
#include "modules/sensor.h"
#include "modules/speed.h"
#include "modules/status_led.h"
#include "modules/storage.h"
#include "modules/wdt.h"

LOG_MODULE_REGISTER(ebike, CONFIG_EBIKE_LOG_LEVEL);

/* ---------- State machine ---------- */

/* New alert at most every N ms while movement continues. */
#define ALARM_REALERT_MS (60 * MSEC_PER_SEC)

enum sm_state_id {
	ST_CONNECT, /* LTE attached, waiting for the AWS broker */
	ST_RIDING,  /* moving: telemetry at full rate */
	ST_PARKED,  /* still: reduced rate; armed when cfg.armed */
	ST_ALARM,   /* armed and moving: alert plus close tracking */
};

struct sm_object {
	struct smf_ctx ctx; /* MUST stay the first member (SMF cast) */

	/* Inputs, refreshed from the zbus channels. */
	bool lte_connected;
	bool wheel_moving;
	bool imu_moving;

	/* Effective configuration (Device Shadow -> CH_CONFIG, NVS backed). */
	struct msg_config cfg;

	/* When movement was last seen (wheel OR IMU). */
	int64_t last_movement_ms;

	/* When the last alert was raised (ALARM state). */
	int64_t last_alert_ms;
};

static const struct smf_state sm_states[];

/* zbus subscriber of the state machine: a notification queue that the main
 * thread drains in the event loop below. */
ZBUS_SUBSCRIBER_DEFINE(sm_sub, 8);
ZBUS_CHAN_ADD_OBS(CH_LTE_STATUS, sm_sub, 3);
ZBUS_CHAN_ADD_OBS(CH_SPEED, sm_sub, 3);
ZBUS_CHAN_ADD_OBS(CH_MOTION, sm_sub, 3);
ZBUS_CHAN_ADD_OBS(CH_CLOUD_CMD, sm_sub, 3);
ZBUS_CHAN_ADD_OBS(CH_CONFIG, sm_sub, 3);

static bool sm_movement(const struct sm_object *sm)
{
	return sm->wheel_moving || sm->imu_moving;
}

static const char *sm_state_name(const struct sm_object *sm)
{
	if (SMF_CTX(sm)->current == &sm_states[ST_RIDING]) {
		return "riding";
	}
	if (SMF_CTX(sm)->current == &sm_states[ST_PARKED]) {
		return "parked";
	}
	if (SMF_CTX(sm)->current == &sm_states[ST_ALARM]) {
		return "alarm";
	}
	return "connect";
}

/* --- CONNECT --- */

static void connect_entry(void *obj)
{
	LOG_INF("State => CONNECT (waiting for the AWS broker)");
}

static enum smf_state_result connect_run(void *obj)
{
	struct sm_object *sm = obj;

	if (cloud_is_connected()) {
		/* Start parked: the first movement switches to RIDING. */
		smf_set_state(SMF_CTX(sm), &sm_states[ST_PARKED]);
	}

	return SMF_EVENT_HANDLED;
}

/* --- RIDING --- */

static void riding_entry(void *obj)
{
	struct sm_object *sm = obj;

	LOG_INF("State => RIDING (rate %u s)", sm->cfg.publish_interval_s);
	sm->last_movement_ms = k_uptime_get();
	location_set_sample_interval(CONFIG_EBIKE_TELEMETRY_SAMPLE_S);
	cloud_set_publish_interval(sm->cfg.publish_interval_s);
	(void)cloud_report_shadow(&sm->cfg, "riding");
}

static enum smf_state_result riding_run(void *obj)
{
	struct sm_object *sm = obj;
	int64_t now = k_uptime_get();

	/* Armed remotely while the bike is moving: that is a theft. */
	if (sm->cfg.armed && sm_movement(sm)) {
		smf_set_state(SMF_CTX(sm), &sm_states[ST_ALARM]);
		return SMF_EVENT_HANDLED;
	}

	if (sm_movement(sm)) {
		sm->last_movement_ms = now;
	} else if (now - sm->last_movement_ms >
		   (int64_t)CONFIG_EBIKE_PARK_TIMEOUT_S * MSEC_PER_SEC) {
		smf_set_state(SMF_CTX(sm), &sm_states[ST_PARKED]);
	}

	return SMF_EVENT_HANDLED;
}

/* --- PARKED --- */

static void parked_entry(void *obj)
{
	struct sm_object *sm = obj;

	LOG_INF("State => PARKED (rate %d s%s)", CONFIG_EBIKE_PUBLISH_PARKED_S,
		sm->cfg.armed ? ", ARMED" : "");
	/* A parked bike has not moved for PARK_TIMEOUT_S by construction, so
	 * recording its position every few seconds only accumulates identical
	 * points that then have to be flushed in a burst — and a burst is
	 * exactly what starves the GNSS and stresses the link. */
	location_set_sample_interval(CONFIG_EBIKE_TELEMETRY_SAMPLE_PARKED_S);
	cloud_set_publish_interval(CONFIG_EBIKE_PUBLISH_PARKED_S);
	(void)cloud_report_shadow(&sm->cfg, "parked");
}

static enum smf_state_result parked_run(void *obj)
{
	struct sm_object *sm = obj;

	if (sm_movement(sm)) {
		smf_set_state(SMF_CTX(sm), sm->cfg.armed
					   ? &sm_states[ST_ALARM]
					   : &sm_states[ST_RIDING]);
	}

	return SMF_EVENT_HANDLED;
}

/* --- ALARM --- */

static void alarm_alert(struct sm_object *sm)
{
	struct msg_alert alert = {
		.cause = ALERT_MOTION_WHILE_ARMED,
		.fix_age_s = -1,
	};
	struct location_fix fix;

	/* Attach the position to the theft alert: this is the alert where it
	 * matters most, and it costs no extra publication. The last known fix
	 * is sent WITH its age — during an alarm the LTE link starves the GNSS,
	 * so the fix may be minutes old and the recipient must be able to
	 * judge. */
	if (location_get_fix(&fix)) {
		alert.has_pos = true;
		alert.lat = fix.lat;
		alert.lon = fix.lon;
		alert.acc_m = fix.accuracy_m;
		alert.fix_age_s = (int32_t)(fix.age_ms / MSEC_PER_SEC);
	}

	sm->last_alert_ms = k_uptime_get();
	/* The cloud listener timestamps and enqueues the alert; the send leaves
	 * on the cloud workqueue, outside the telemetry cycle. With the link
	 * down it stays queued and is replayed with its age, so a theft that
	 * happens out of coverage is no longer lost. */
	(void)zbus_chan_pub(&CH_ALERT, &alert, K_MSEC(100));
}

static void alarm_entry(void *obj)
{
	struct sm_object *sm = obj;

	LOG_WRN("State => ALARM (movement while armed) — tracking %u s",
		sm->cfg.publish_interval_s);
	/* Theft in progress: back to fine resolution, this is when the trace
	 * matters most. */
	location_set_sample_interval(CONFIG_EBIKE_TELEMETRY_SAMPLE_S);
	alarm_alert(sm);
	cloud_set_publish_interval(sm->cfg.publish_interval_s);
	(void)cloud_report_shadow(&sm->cfg, "alarm");
}

static enum smf_state_result alarm_run(void *obj)
{
	struct sm_object *sm = obj;

	/* Only a remote disarm (shadow desired.armed=false) leaves this state:
	 * a stolen bike that stopped moving stays in alarm. */
	if (!sm->cfg.armed) {
		LOG_INF("Disarmed remotely — end of alert");
		smf_set_state(SMF_CTX(sm), &sm_states[ST_PARKED]);
		return SMF_EVENT_HANDLED;
	}

	/* Periodic reminder while movement continues. */
	if (sm_movement(sm) &&
	    (k_uptime_get() - sm->last_alert_ms) > ALARM_REALERT_MS) {
		alarm_alert(sm);
	}

	return SMF_EVENT_HANDLED;
}

static const struct smf_state sm_states[] = {
	[ST_CONNECT] = SMF_CREATE_STATE(connect_entry, connect_run, NULL,
					NULL, NULL),
	[ST_RIDING] = SMF_CREATE_STATE(riding_entry, riding_run, NULL,
				       NULL, NULL),
	[ST_PARKED] = SMF_CREATE_STATE(parked_entry, parked_run, NULL,
				       NULL, NULL),
	[ST_ALARM] = SMF_CREATE_STATE(alarm_entry, alarm_run, NULL,
				      NULL, NULL),
};

/* One-shot command received on bike/<id>/cmd.
 * {"set_home":true} makes the current GNSS position the geofence reference,
 * which saves entering lat/lon by hand in the shadow. */
static void sm_handle_cloud_cmd(struct sm_object *sm, const char *json)
{
	cJSON *root = cJSON_Parse(json);
	cJSON *item;

	if (root == NULL) {
		return;
	}

	item = cJSON_GetObjectItem(root, "set_home");
	if (cJSON_IsTrue(item) ||
	    (cJSON_IsNumber(item) && item->valueint != 0)) {
		struct location_fix fix;

		if (!location_get_fix(&fix)) {
			LOG_WRN("set_home: no GNSS fix — command ignored");
		} else {
			sm->cfg.home_lat = fix.lat;
			sm->cfg.home_lon = fix.lon;
			sm->cfg.home_set = true;
			(void)storage_set_home(fix.lat, fix.lon);
			(void)cloud_desire_home(fix.lat, fix.lon);
			geofence_apply_config(&sm->cfg);
			LOG_INF("Geofence: home set here — %.6f, %.6f "
				"(+/-%.0f m)", fix.lat, fix.lon,
				(double)fix.accuracy_m);
			/* Republish the merged configuration so the next shadow
			 * deltas start from this base; the report to AWS leaves
			 * when the CH_CONFIG notification comes back. */
			(void)zbus_chan_pub(&CH_CONFIG, &sm->cfg, K_MSEC(100));
		}
	}

	cJSON_Delete(root);
}

/* Refresh the state machine inputs from the channel that just published. */
static void sm_consume(struct sm_object *sm, const struct zbus_channel *chan)
{
	if (chan == &CH_SPEED) {
		struct msg_speed msg;

		if (zbus_chan_read(&CH_SPEED, &msg, K_MSEC(100)) == 0) {
			sm->wheel_moving = msg.moving;
		}
	} else if (chan == &CH_MOTION) {
		struct msg_motion msg;

		if (zbus_chan_read(&CH_MOTION, &msg, K_MSEC(100)) == 0) {
			sm->imu_moving = msg.moving;
		}
	} else if (chan == &CH_LTE_STATUS) {
		struct msg_lte_status msg;

		if (zbus_chan_read(&CH_LTE_STATUS, &msg, K_MSEC(100)) == 0) {
			if (sm->lte_connected && !msg.connected) {
				LOG_WRN("LTE lost — lte_lc is reconnecting");
			}
			sm->lte_connected = msg.connected;
		}
	} else if (chan == &CH_CLOUD_CMD) {
		struct msg_cloud_cmd msg;

		if (zbus_chan_read(&CH_CLOUD_CMD, &msg, K_MSEC(100)) == 0) {
			/* Arm and disarm go through the shadow; the cmd topic
			 * carries one-shot commands such as set_home. */
			LOG_INF("Cloud command received: %s", msg.json);
			sm_handle_cloud_cmd(sm, msg.json);
		}
	} else if (chan == &CH_CONFIG) {
		struct msg_config cfg;

		if (zbus_chan_read(&CH_CONFIG, &cfg, K_MSEC(100)) != 0) {
			return;
		}

		/* Apply and persist the changes, then report back to the
		 * shadow, which settles the delta on the AWS side. */
		if (cfg.armed != sm->cfg.armed) {
			LOG_WRN("Anti-theft %s (via shadow)",
				cfg.armed ? "ARMED" : "disarmed");
			(void)storage_set_armed(cfg.armed);
		}
		if (cfg.publish_interval_s != sm->cfg.publish_interval_s) {
			(void)storage_set_publish_interval_s(
				cfg.publish_interval_s);
			/* Active rate in RIDING/ALARM: apply immediately. */
			if (SMF_CTX(sm)->current == &sm_states[ST_RIDING] ||
			    SMF_CTX(sm)->current == &sm_states[ST_ALARM]) {
				cloud_set_publish_interval(
					cfg.publish_interval_s);
			}
		}
		if (cfg.motion_threshold_mg != sm->cfg.motion_threshold_mg) {
			(void)storage_set_motion_threshold_mg(
				cfg.motion_threshold_mg);
			imu_set_motion_threshold_mg(cfg.motion_threshold_mg);
		}
		if (cfg.home_set != sm->cfg.home_set ||
		    cfg.home_lat != sm->cfg.home_lat ||
		    cfg.home_lon != sm->cfg.home_lon) {
			(void)storage_set_home(cfg.home_lat, cfg.home_lon);
			LOG_INF("Geofence: home %.6f, %.6f (via shadow)",
				cfg.home_lat, cfg.home_lon);
		}
		if (cfg.geofence_radius_m != sm->cfg.geofence_radius_m) {
			(void)storage_set_geofence_radius_m(
				cfg.geofence_radius_m);
		}
		if (cfg.geofence_enabled != sm->cfg.geofence_enabled) {
			(void)storage_set_geofence_enabled(
				cfg.geofence_enabled);
			LOG_WRN("Geofence %s (via shadow)",
				cfg.geofence_enabled ? "ENABLED" : "disabled");
		}
		/* Self-diffing: no effect when the geofence fields are
		 * unchanged, so a plain arm/disarm does not reset the zone. */
		geofence_apply_config(&cfg);

		sm->cfg = cfg;
		(void)cloud_report_shadow(&sm->cfg, sm_state_name(sm));
	}
}

/* ---------- Boot and event loop ---------- */

int main(void)
{
	struct sm_object sm = {0};
	int err;

	/* Raw printk, independent of the logging subsystem, so a board that
	 * boots can be told apart from one that does not. */
	printk("\n>>> eBike: main() started <<<\n");

	LOG_INF("=================================================");
	LOG_INF(" eBike tracker firmware");
	LOG_INF(" Board: %s", CONFIG_BOARD);
	LOG_INF("=================================================");

	/* Status LEDs FIRST: on battery there is no console, and every boot
	 * step below (modem init, credential provisioning, LTE attach) is long
	 * enough to stall — the heartbeat has to be blinking while they run,
	 * otherwise a stalled boot is indistinguishable from a dead board. */
	(void)status_led_init();

	/* Reset cause BEFORE anything else: this is what makes repeated field
	 * reboots diagnosable (published in the status). */
	(void)diag_init();

	err = connectivity_init();
	if (err) {
		LOG_ERR("Connectivity init failed (%d) — stopping.", err);
		return 0;
	}

	/* TLS credentials into the modem: needs the CFUN=0 window, before the
	 * attach. */
	err = cloud_provision_credentials();
	if (err) {
		LOG_ERR("AWS credential provisioning failed (%d) — stopping.",
			err);
		return 0;
	}

	err = connectivity_connect();
	if (err) {
		LOG_ERR("LTE connection failed (%d) — stopping.", err);
		return 0;
	}

	LOG_INF("Waiting for network attach...");

	/* Non-blocking wait with progress logs: do not give up on the first
	 * timeout, since a freshly activated SIM can take minutes to be
	 * authorised on a roaming network. */
	while (!connectivity_is_connected()) {
		if (connectivity_wait_for_connection(20 * 1000) == 0) {
			break;
		}
		connectivity_log_status();
	}
	sm.lte_connected = true;

	LOG_INF("Modem registered — IP address obtained.");

	/* AWS IoT broker connection (asynchronous, retries internally). */
	err = cloud_init();
	if (err) {
		LOG_ERR("Cloud init failed (%d) — stopping.", err);
		return 0;
	}
	(void)cloud_connect();

	/* GNSS: periodic fixes coexisting with LTE. */
	err = location_start();
	if (err) {
		LOG_WRN("GNSS unavailable (%d) — continuing without position.",
			err);
	}

	/* IMU: accelerometer polling (X-NUCLEO-IKS01A1 shield). */
	err = imu_init();
	if (err) {
		LOG_WRN("IMU unavailable (%d) — continuing without "
			"acceleration.", err);
	}

	/* Persistent configuration (NVS) then the wheel speed sensor. */
	err = storage_init();
	if (err) {
		LOG_WRN("NVS unavailable (%d) — using Kconfig defaults.", err);
	}
	speed_set_wheel_circ_mm(storage_get_wheel_circ_mm());
	err = speed_init();
	if (err) {
		LOG_WRN("Speed sensor unavailable (%d) — continuing without.",
			err);
	}

	/* Persisted anti-theft and geofence configuration, reloaded at boot: a
	 * bike stolen with the battery disconnected comes back up ARMED, zone
	 * included. Published on CH_CONFIG so shadow merges start from the
	 * right base. */
	sm.cfg = (struct msg_config){
		.armed = storage_get_armed(),
		.publish_interval_s = storage_get_publish_interval_s(),
		.motion_threshold_mg = storage_get_motion_threshold_mg(),
		.geofence_radius_m = storage_get_geofence_radius_m(),
		.geofence_enabled = storage_get_geofence_enabled(),
	};
	sm.cfg.home_set = storage_get_home(&sm.cfg.home_lat, &sm.cfg.home_lon);
	imu_set_motion_threshold_mg(sm.cfg.motion_threshold_mg);
	geofence_apply_config(&sm.cfg);
	(void)zbus_chan_pub(&CH_CONFIG, &sm.cfg, K_MSEC(100));
	if (sm.cfg.armed) {
		LOG_WRN("Booting ARMED (persisted state) — any movement will "
			"alert");
	}

	/* Watchdog LAST, once every watched loop is running: the nRF91
	 * watchdog can no longer be stopped once started, so it must never run
	 * during an incomplete boot (a long LTE attach or certificate
	 * provisioning feeds nothing). A failure here is not fatal — a bike
	 * without a watchdog beats a bike that does not start. */
	err = wdt_init();
	if (err) {
		LOG_WRN("Watchdog unavailable (%d) — hangs unmonitored.", err);
	}

	/* ---- State machine takes over ---- */
	smf_set_initial(SMF_CTX(&sm), &sm_states[ST_CONNECT]);

	while (1) {
		const struct zbus_channel *chan = NULL;

		/* Proof of life for the event loop: it comes back here at least
		 * every 5 s (the timeout below), well under the channel
		 * timeout. */
		wdt_feed_main();

		/* Woken by any observed publication (sensor tickers, about
		 * once a second); the fallback timeout keeps the state machine
		 * running if the producers are silent (missing sensors). */
		if (zbus_sub_wait(&sm_sub, &chan, K_SECONDS(5)) == 0) {
			sm_consume(&sm, chan);
		}

		if (smf_run_state(SMF_CTX(&sm))) {
			LOG_ERR("State machine terminated — stopping.");
			break;
		}
	}

	return 0;
}
