/*
 * eBike tracker — cloud module implementation.
 */

#include "cloud.h"
#include "assistance.h"
#include "connectivity.h"
#include "diag.h"
#include "location.h"
#include "wdt.h"
#include "events/channels.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <net/aws_iot.h>

#include <cJSON.h>
#include <cJSON_os.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(cloud, CONFIG_EBIKE_LOG_LEVEL);

/* The broker endpoint is account specific and has no sensible default, so
 * catch it at build time rather than let the device fail to connect forever.
 * See firmware/local.conf.example. */
BUILD_ASSERT(sizeof(CONFIG_AWS_IOT_BROKER_HOST_NAME) > 1,
	     "CONFIG_AWS_IOT_BROKER_HOST_NAME is empty. Copy "
	     "firmware/local.conf.example to firmware/local.conf, set your AWS "
	     "IoT endpoint, and build with -DEXTRA_CONF_FILE=local.conf");

#define TOPIC_STATUS    "bike/" CONFIG_AWS_IOT_CLIENT_ID_STATIC "/status"
#define TOPIC_TELEMETRY "bike/" CONFIG_AWS_IOT_CLIENT_ID_STATIC "/telemetry"
#define TOPIC_CMD       "bike/" CONFIG_AWS_IOT_CLIENT_ID_STATIC "/cmd"
#define TOPIC_ALERT     "bike/" CONFIG_AWS_IOT_CLIENT_ID_STATIC "/alert"

/* ---------- Dedicated cloud workqueue -------------------------------------
 *
 * connect_work and publish_work used to run on the SYSTEM workqueue, shared
 * with GNSS PVT frames, the IMU poll and the speed ticker. That had two
 * consequences, both observed:
 *
 *  - head-of-line blocking: a slow aws_iot_connect() (DNS plus TLS handshake
 *    on a degraded network) holds up PVT processing behind it, exactly when
 *    GNSS needs it most — during a network recovery;
 *
 *  - an unusable watchdog: watching the system workqueue would have required
 *    a timeout sized for the worst TLS handshake, which amounts to watching
 *    nothing. Separated, each queue gets the timeout its work deserves.
 */
static K_THREAD_STACK_DEFINE(cloud_stack, CONFIG_EBIKE_CLOUD_STACK_SIZE);
static struct k_work_q cloud_wq;

static void connect_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

static void publish_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(publish_work, publish_work_fn);

/* Alert queue flush, woken on every enqueue (see alert_flush()). */
static void alert_work_fn(struct k_work *work);
static K_WORK_DEFINE(alert_work, alert_work_fn);

/* Publication rate (s), set by the state machine (0 = not yet). */
static atomic_t publish_interval_s;

static volatile bool is_connected;

/* Reconnection backoff: current delay, doubled on every failure, capped, and
 * reset to the minimum as soon as a connection succeeds. */
static uint32_t backoff_s = CONFIG_EBIKE_CLOUD_BACKOFF_MIN_S;

/* LTE attach polling. This is NOT part of the cloud backoff: testing a boolean
 * in RAM costs neither radio nor energy, and the return of the network
 * triggers an immediate attempt anyway (CH_LTE_STATUS listener). */
#define LTE_WAIT_POLL_S 30

/* Consecutive connection failures, reset as soon as a connection succeeds.
 * This is the only counter able to tell "no network" (normal, keep waiting)
 * from "modem resource stuck" (abnormal, only a reboot recovers) — see
 * connect_escalate(). */
static uint32_t connect_failures;

/* ---------- MQTT traffic regulation ---------------------------------------
 *
 * Four state transitions in 105 s once produced eight off-cycle publications
 * (an immediate status plus a shadow report each time). The modem stayed in
 * RRC connected throughout and the GNSS got no RF window for nearly seven
 * minutes. Priority mode does not help here — the modem API is explicit that
 * it will not stop LTE connections.
 *
 * Hence: debounced transition publications, grouped shadow reports, and a
 * periodic shadow pull (see shadow_get_if_due()).
 */
static int64_t last_publish_ms = -1;
static int64_t last_shadow_ms = -1;
static int64_t last_shadow_get_ms = -1;

/* Shadow report deferred by the debounce: the configuration is kept as is and
 * sent on the next cycle. The reported state stays correct, it just arrives
 * grouped. */
static bool shadow_report_due;
static struct msg_config shadow_cfg;

/* View of the state machine, refreshed on every cloud_report_shadow(), so the
 * status can carry sm_state/armed without a dependency on main.c. State names
 * are string literals with static lifetime, so storing the pointer is safe. */
static const char *sm_state_str = "connect";
static bool sm_armed;

/* ---------- Reconnection backoff ------------------------------------------
 *
 * Retrying every 30 s forever burns energy for nothing in a prolonged dead
 * zone: every attempt wakes the radio and starts a TLS handshake that cannot
 * succeed. Exponential backoff spaces the attempts out to a cap, and the
 * jitter keeps the reconnection cycle from locking onto the publication cycle
 * — two radio wake-ups that always overlap would starve the GNSS.
 *
 * The jitter comes from the cycle counter rather than sys_rand32_get():
 * randomness quality is irrelevant here, and this avoids forcing an entropy
 * dependency just to desynchronise two timers.
 */
static uint32_t backoff_with_jitter(void)
{
	uint32_t span = backoff_s / 2; /* +/-25% window */
	uint32_t jitter = span > 0 ? (k_cycle_get_32() % (span + 1)) : 0;
	uint32_t delay = backoff_s - (span / 2) + jitter;

	/* The jitter must not push past the cap: at the top value, +25% would
	 * exceed the maximum the configuration promises. */
	return MIN(delay, (uint32_t)CONFIG_EBIKE_CLOUD_BACKOFF_MAX_S);
}

static void backoff_bump(void)
{
	uint32_t next = backoff_s * 2;

	backoff_s = MIN(next, (uint32_t)CONFIG_EBIKE_CLOUD_BACKOFF_MAX_S);
}

static void backoff_reset(void)
{
	backoff_s = CONFIG_EBIKE_CLOUD_BACKOFF_MIN_S;
}

/* ---------- Offline alert queue -------------------------------------------
 *
 * Alerts used to be sent off-cycle, IN THE PUBLISHER'S CONTEXT. Two defects,
 * both fixed by this queue:
 *
 *  1. With the link down, publish_json() returned -ENOTCONN and the alert was
 *     LOST. A theft detected during an outage left no trace at all, no e-mail
 *     and no record — the most critical message in the system was the only one
 *     with no safety net.
 *
 *  2. A geofence alert is published by a zbus listener on CH_GNSS_FIX, which
 *     runs in the SYSTEM workqueue. Sending MQTT/TLS inline there blocked GNSS
 *     and the speed ticker for the duration of the socket write, and made a
 *     tight watchdog timeout on that queue impossible.
 *
 * All alerts therefore go through this queue, drained on the cloud workqueue
 * and woken immediately on every enqueue: sending stays immediate while the
 * link is up and becomes a replay when it returns. The age travels with the
 * alert — without it the notifier would date a replayed alert at catch-up
 * time and announce a theft "just now" that happened twenty minutes earlier.
 */

struct pending_alert {
	struct msg_alert alert;
	int64_t uptime_ms; /* when the alert was raised */
};

static struct pending_alert alert_q[CONFIG_EBIKE_ALERT_QUEUE_SIZE];
static int alert_head;  /* next write slot */
static int alert_count;
static struct k_spinlock alert_lock;

/* @p uptime_ms is passed explicitly rather than read here, so that requeueing
 * an alert whose replay failed keeps its original time — otherwise every retry
 * would rejuvenate it and the e-mail would eventually date it at the moment
 * the send succeeded. */
static void alert_enqueue(const struct msg_alert *alert, int64_t uptime_ms)
{
	K_SPINLOCK(&alert_lock) {
		alert_q[alert_head] = (struct pending_alert){
			.alert = *alert,
			.uptime_ms = uptime_ms,
		};
		alert_head = (alert_head + 1) % CONFIG_EBIKE_ALERT_QUEUE_SIZE;
		if (alert_count < CONFIG_EBIKE_ALERT_QUEUE_SIZE) {
			alert_count++;
		}
		/* Queue full: the OLDEST is overwritten. That is the right
		 * trade-off — in a burst of geofence reminders, the most recent
		 * ones describe better where the bike is now. */
	}
}

/* Non-destructive read of the oldest: the alert is only removed (alert_drop)
 * once actually published. Without that split, a failure would force a
 * requeue at the TAIL, putting it behind newer alerts and scrambling the
 * chronological order on every failed attempt. */
static bool alert_peek(struct pending_alert *out)
{
	bool got = false;

	K_SPINLOCK(&alert_lock) {
		if (alert_count > 0) {
			int idx = (alert_head - alert_count +
				   CONFIG_EBIKE_ALERT_QUEUE_SIZE) %
				  CONFIG_EBIKE_ALERT_QUEUE_SIZE;

			*out = alert_q[idx];
			got = true;
		}
	}

	return got;
}

static void alert_drop(void)
{
	K_SPINLOCK(&alert_lock) {
		if (alert_count > 0) {
			alert_count--;
		}
	}
}

static int alert_pending(void)
{
	int n;

	K_SPINLOCK(&alert_lock) {
		n = alert_count;
	}

	return n;
}

/* ---------- Credential provisioning into the modem ---------- */

#if defined(CONFIG_EBIKE_PROVISION_CERTS)

#include <modem/modem_key_mgmt.h>

/* PEM files embedded in the image at build time (from firmware/certs/, not
 * versioned). Development only: provision out of band in production. */
static const uint8_t aws_ca_cert[] = {
#include "AmazonRootCA1.pem.inc"
};
static const uint8_t aws_device_cert[] = {
#include "device-cert.pem.inc"
};
static const uint8_t aws_private_key[] = {
#include "private.key.inc"
};

static int provision_one(enum modem_key_mgmt_cred_type type, const char *label,
			 const uint8_t *buf, size_t len)
{
	int err;
	bool exists;

	/* Only rewrite when missing or different, to preserve the modem NVM. */
	err = modem_key_mgmt_cmp(CONFIG_MQTT_HELPER_SEC_TAG, type, buf, len);
	if (err == 0) {
		LOG_INF("%s: already provisioned (sec_tag %d)", label,
			CONFIG_MQTT_HELPER_SEC_TAG);
		return 0;
	}

	/* -EACCES means the modem refuses to read that credential back (AT
	 * error 514). A private key never is readable, and some modem firmware
	 * versions refuse the device certificate too, so the comparison above
	 * can NEVER succeed for those two. Treating that as "it differs" would
	 * rewrite both into the modem NVM on EVERY boot: flash wear and a
	 * slower boot, for a credential that was already correct.
	 *
	 * Existence is the strongest answer obtainable here, and it is the
	 * right one as long as the credential is not being rotated — which is
	 * what EBIKE_PROVISION_FORCE is for. */
	if (err == -EACCES && !IS_ENABLED(CONFIG_EBIKE_PROVISION_FORCE)) {
		if (modem_key_mgmt_exists(CONFIG_MQTT_HELPER_SEC_TAG, type,
					  &exists) == 0 && exists) {
			LOG_INF("%s: present, contents not readable — kept "
				"(sec_tag %d)", label,
				CONFIG_MQTT_HELPER_SEC_TAG);
			return 0;
		}
	}

	err = modem_key_mgmt_write(CONFIG_MQTT_HELPER_SEC_TAG, type, buf, len);
	if (err) {
		LOG_ERR("Writing %s failed: %d", label, err);
		return err;
	}

	LOG_INF("%s provisioned (sec_tag %d)", label,
		CONFIG_MQTT_HELPER_SEC_TAG);
	return 0;
}

int cloud_provision_credentials(void)
{
	int err;

	err = provision_one(MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, "Root CA",
			    aws_ca_cert, sizeof(aws_ca_cert));
	if (err) {
		return err;
	}

	err = provision_one(MODEM_KEY_MGMT_CRED_TYPE_PUBLIC_CERT,
			    "Device certificate", aws_device_cert,
			    sizeof(aws_device_cert));
	if (err) {
		return err;
	}

	return provision_one(MODEM_KEY_MGMT_CRED_TYPE_PRIVATE_CERT,
			     "Private key", aws_private_key,
			     sizeof(aws_private_key));
}

#else /* CONFIG_EBIKE_PROVISION_CERTS */

int cloud_provision_credentials(void)
{
	return 0;
}

#endif /* CONFIG_EBIKE_PROVISION_CERTS */

/* ---------- Device Shadow ----------
 * Reception: `get/accepted` (full document, requested automatically on every
 * connection) and `update/delta` (desired diverging from reported). Recognised
 * fields are merged into CH_CONFIG (read-modify-publish: a delta carries ONLY
 * the fields that change). The state machine consumes them, applies them,
 * persists them in NVS and then reports through cloud_report_shadow(), which
 * settles the delta on the AWS side. */

static void shadow_config_handle(const char *json, size_t len, bool full_doc)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	cJSON *state, *src, *item;
	struct msg_config cfg;
	bool changed = false;

	if (root == NULL) {
		LOG_WRN("Shadow: invalid JSON");
		return;
	}

	/* Full document: state.desired. Delta: state directly. */
	state = cJSON_GetObjectItem(root, "state");
	src = full_doc ? cJSON_GetObjectItem(state, "desired") : state;
	if (src == NULL) {
		/* Empty shadow (never configured): nothing to apply. */
		cJSON_Delete(root);
		return;
	}

	if (zbus_chan_read(&CH_CONFIG, &cfg, K_MSEC(100)) != 0) {
		cJSON_Delete(root);
		return;
	}

	item = cJSON_GetObjectItem(src, "armed");
	if (cJSON_IsBool(item)) {
		cfg.armed = cJSON_IsTrue(item);
		changed = true;
	} else if (cJSON_IsNumber(item)) {
		cfg.armed = (item->valueint != 0);
		changed = true;
	}

	item = cJSON_GetObjectItem(src, "publish_interval_s");
	if (cJSON_IsNumber(item) && item->valueint > 0) {
		cfg.publish_interval_s = (uint32_t)item->valueint;
		changed = true;
	}

	item = cJSON_GetObjectItem(src, "motion_threshold_mg");
	if (cJSON_IsNumber(item) && item->valueint > 0) {
		cfg.motion_threshold_mg = (uint32_t)item->valueint;
		changed = true;
	}

	/* Geofence. home_lat/home_lon must be set TOGETHER: a delta carrying
	 * only one of them merges with the current value of the other. */
	item = cJSON_GetObjectItem(src, "home_lat");
	if (cJSON_IsNumber(item)) {
		cfg.home_lat = item->valuedouble;
		cfg.home_set = true;
		changed = true;
	}

	item = cJSON_GetObjectItem(src, "home_lon");
	if (cJSON_IsNumber(item)) {
		cfg.home_lon = item->valuedouble;
		cfg.home_set = true;
		changed = true;
	}

	item = cJSON_GetObjectItem(src, "geofence_radius_m");
	if (cJSON_IsNumber(item) && item->valueint > 0) {
		cfg.geofence_radius_m = (uint32_t)item->valueint;
		changed = true;
	}

	item = cJSON_GetObjectItem(src, "geofence_enabled");
	if (cJSON_IsBool(item)) {
		cfg.geofence_enabled = cJSON_IsTrue(item);
		changed = true;
	} else if (cJSON_IsNumber(item)) {
		cfg.geofence_enabled = (item->valueint != 0);
		changed = true;
	}

	cJSON_Delete(root);

	if (changed) {
		LOG_INF("Shadow %s: armed=%d, interval=%u s, threshold=%u mg, "
			"geofence=%d r=%u m home=%.6f,%.6f",
			full_doc ? "(desired)" : "(delta)",
			cfg.armed, cfg.publish_interval_s,
			cfg.motion_threshold_mg, cfg.geofence_enabled,
			cfg.geofence_radius_m, cfg.home_lat, cfg.home_lon);
		(void)zbus_chan_pub(&CH_CONFIG, &cfg, K_MSEC(100));
	}
}

/* Last report actually sent, to drop an identical repeat (see below). Cleared
 * on disconnection so the first report of a new session always goes out. */
static char last_shadow_json[320];

static int shadow_report_send(const struct msg_config *cfg, const char *sm_state)
{
	char json[320];
	int err;
	struct aws_iot_data msg = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic.type = AWS_IOT_SHADOW_TOPIC_UPDATE,
	};

	if (!is_connected) {
		return -ENOTCONN;
	}

	/* Every field acceptable in desired MUST appear here, otherwise its
	 * delta is never settled on the AWS side (home reported as 0,0 while
	 * undefined). */
	snprintf(json, sizeof(json),
		 "{\"state\":{\"reported\":{\"armed\":%s,"
		 "\"publish_interval_s\":%u,\"motion_threshold_mg\":%u,"
		 "\"geofence_enabled\":%s,\"geofence_radius_m\":%u,"
		 "\"home_lat\":%.6f,\"home_lon\":%.6f,"
		 "\"sm_state\":\"%s\"}}}",
		 cfg->armed ? "true" : "false",
		 cfg->publish_interval_s, cfg->motion_threshold_mg,
		 cfg->geofence_enabled ? "true" : "false",
		 cfg->geofence_radius_m, cfg->home_lat, cfg->home_lon,
		 sm_state);

	/* Identical to the last one sent: there is nothing left to settle on
	 * the AWS side, so the publish would buy nothing. It happened twice at
	 * every boot and on every transition — the direct report, then the one
	 * the debounce had deferred and publish_work_fn flushed a moment later,
	 * byte for byte the same. That is ~300 bytes plus a PUBACK of radio
	 * time, in a firmware built around not waking the radio for nothing.
	 * `sm_state` is part of the payload, so any state change still gets
	 * through. */
	if (strcmp(json, last_shadow_json) == 0) {
		last_shadow_ms = k_uptime_get();
		shadow_report_due = false;
		LOG_DBG("Shadow report unchanged — not republished");
		return 0;
	}

	msg.ptr = json;
	msg.len = strlen(json);

	err = aws_iot_send(&msg);
	if (err) {
		LOG_ERR("Shadow report: %d", err);
		return err;
	}

	strncpy(last_shadow_json, json, sizeof(last_shadow_json) - 1);
	last_shadow_json[sizeof(last_shadow_json) - 1] = '\0';
	last_shadow_ms = k_uptime_get();
	shadow_report_due = false;
	LOG_INF("AWS IoT -> [shadow/update]: %s", json);

	return 0;
}

int cloud_report_shadow(const struct msg_config *cfg, const char *sm_state)
{
	int64_t now = k_uptime_get();

	/* Always remember, even offline or deferred: this view feeds
	 * sm_state/armed in the status and the grouped report. */
	shadow_cfg = *cfg;
	sm_state_str = sm_state;
	sm_armed = cfg->armed;

	if (!is_connected) {
		shadow_report_due = true;
		return -ENOTCONN;
	}

	/* Debounce: a report too close to the previous one is held back and
	 * leaves with the next status publication. */
	if (CONFIG_EBIKE_SHADOW_REPORT_MIN_S > 0 && last_shadow_ms >= 0 &&
	    (now - last_shadow_ms) <
		    (int64_t)CONFIG_EBIKE_SHADOW_REPORT_MIN_S * MSEC_PER_SEC) {
		shadow_report_due = true;
		LOG_DBG("Shadow report deferred (debounce)");
		return 0;
	}

	return shadow_report_send(cfg, sm_state);
}

/* Re-read the Device Shadow with an empty publication on shadow/get.
 *
 * WHY A PULL: a delta is a DOWNLINK message, and with PSM the bike is only
 * reachable during the short window that follows its own transmissions, while
 * the AWS-side TCP retransmission backs off exponentially — a command could
 * therefore wait several minutes. A `get` is sent by the device, so the answer
 * (`get/accepted`, handled by the same path as the boot document) comes back
 * while the RRC connection is still open. The wait is then bounded by
 * CONFIG_EBIKE_SHADOW_POLL_S.
 */
static void shadow_get_if_due(int64_t now)
{
	struct aws_iot_data msg = {
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic.type = AWS_IOT_SHADOW_TOPIC_GET,
		.ptr = "",
		.len = 0,
	};
	int err;

	if (CONFIG_EBIKE_SHADOW_POLL_S == 0) {
		return;
	}
	if (last_shadow_get_ms >= 0 &&
	    (now - last_shadow_get_ms) <
		    (int64_t)CONFIG_EBIKE_SHADOW_POLL_S * MSEC_PER_SEC) {
		return;
	}

	err = aws_iot_send(&msg);
	if (err) {
		LOG_WRN("shadow/get: %d", err);
		return;
	}

	last_shadow_get_ms = now;
	LOG_DBG("AWS IoT -> [shadow/get] (re-reading commands)");
}

int cloud_desire_home(double lat, double lon)
{
	char json[128];
	int err;
	struct aws_iot_data msg = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic.type = AWS_IOT_SHADOW_TOPIC_UPDATE,
	};

	if (!is_connected) {
		return -ENOTCONN;
	}

	/* The desired state is the cloud backup of the home point: NVS does not
	 * survive the mass erase of `west flash --recover`, the shadow does,
	 * and the next boot restores home_lat/home_lon from get/accepted. */
	snprintf(json, sizeof(json),
		 "{\"state\":{\"desired\":{"
		 "\"home_lat\":%.6f,\"home_lon\":%.6f}}}",
		 lat, lon);

	msg.ptr = json;
	msg.len = strlen(json);

	err = aws_iot_send(&msg);
	if (err) {
		LOG_ERR("Desire home: %d", err);
		return err;
	}

	LOG_INF("AWS IoT -> [shadow/update]: %s", json);

	return 0;
}

/* ---------- AWS IoT connection ---------- */

/* Last resort when connecting fails in a loop.
 *
 * After an abrupt RF loss, aws_iot_connect() can return -ENOMEM on EVERY
 * attempt, indefinitely: a TLS socket stays open on the modem side and its
 * pool never frees up. The bike is then mute for good, and the exponential
 * backoff only makes the failure quieter.
 *
 * No API can force the cleanup from the disconnected state
 * (mqtt_helper_disconnect() requires CONNECTED). A reboot is the only way to
 * recover, and it beats a silent tracker: the `boot` field of the status makes
 * it visible afterwards.
 */
static void connect_escalate(void)
{
	if (CONFIG_EBIKE_CLOUD_MAX_CONNECT_FAILURES == 0 ||
	    connect_failures < CONFIG_EBIKE_CLOUD_MAX_CONNECT_FAILURES) {
		return;
	}

	LOG_ERR("%u consecutive connection failures — modem resource likely "
		"stuck, restarting", connect_failures);

	/* Flush deferred logs, otherwise the message above goes down with the
	 * reset and the next boot is inexplicable from the console. */
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
}

static void connect_work_fn(struct k_work *work)
{
	uint32_t delay;
	int err;

	/* Without LTE attached, aws_iot_connect() is doomed: it would wake the
	 * radio and burn a handshake for nothing. Come back later instead — the
	 * return of the network triggers an immediate attempt through the
	 * CH_LTE_STATUS listener, this poll is only a safety net. The cloud
	 * backoff is not incremented: the failure is not its own. */
	if (!connectivity_is_connected()) {
		LOG_DBG("LTE not attached — cloud connection postponed");
		(void)k_work_reschedule_for_queue(&cloud_wq, &connect_work,
						  K_SECONDS(LTE_WAIT_POLL_S));
		return;
	}

	LOG_INF("Connecting to AWS IoT (%s)...",
		CONFIG_AWS_IOT_BROKER_HOST_NAME);

	/* NULL: static client id and endpoint from Kconfig. */
	err = aws_iot_connect(NULL);
	if (err == 0) {
		return; /* AWS_IOT_EVT_CONNECTED will reset the backoff */
	}

	/* Counted HERE and not in the disconnect handler: only a failed ATTEMPT
	 * while LTE is attached signals an anomaly. A dead zone leaves through
	 * the guard above without touching the counter. */
	connect_failures++;
	backoff_bump();
	delay = backoff_with_jitter();

	if (err == -EAGAIN) {
		LOG_INF("Connection timeout (%u) — retrying in %u s",
			connect_failures, delay);
	} else {
		LOG_ERR("aws_iot_connect: %d (failure %u/%d) — retrying in %u s",
			err, connect_failures,
			CONFIG_EBIKE_CLOUD_MAX_CONNECT_FAILURES, delay);
	}

	connect_escalate();

	(void)k_work_reschedule_for_queue(&cloud_wq, &connect_work,
					  K_SECONDS(delay));
}

/* LTE is back: do not wait for the next backoff tick. The listener runs in the
 * connectivity context (lte_lc handler), so it only queues work, never blocks. */
static void lte_listener_cb(const struct zbus_channel *chan)
{
	const struct msg_lte_status *lte = zbus_chan_const_msg(chan);

	if (!lte->connected) {
		/* LTE lost: restart the failure counter from zero. This is THE
		 * discriminant of the escalation — a failure while coverage
		 * comes and goes (a tunnel, a gap while riding) says nothing
		 * about the modem state, whereas a run of failures with LTE
		 * CONTINUOUSLY attached can only be a stuck resource. Without
		 * this reset, a ride through patchy coverage would eventually
		 * trigger a perfectly useless reboot mid-trip. */
		connect_failures = 0;
		return;
	}

	if (is_connected) {
		return;
	}

	LOG_INF("LTE back — immediate cloud reconnection attempt");
	backoff_reset();
	(void)k_work_reschedule_for_queue(&cloud_wq, &connect_work, K_NO_WAIT);
}

ZBUS_LISTENER_DEFINE(cloud_lte_lis, lte_listener_cb);
ZBUS_CHAN_ADD_OBS(CH_LTE_STATUS, cloud_lte_lis, 4);

static void aws_iot_evt_handler(const struct aws_iot_evt *const evt)
{
	switch (evt->type) {
	case AWS_IOT_EVT_CONNECTING:
		LOG_INF("AWS IoT: connecting...");
		break;

	case AWS_IOT_EVT_CONNECTED:
		is_connected = true;
		backoff_reset();
		connect_failures = 0;
		(void)k_work_cancel_delayable(&connect_work);
		/* CONFIG_AWS_IOT_AUTO_DEVICE_SHADOW_REQUEST already issues a
		 * shadow/get on every connection, so push our own pull out by a
		 * full period; otherwise it would fire on the first publication
		 * cycle and the full document would arrive twice. */
		last_shadow_get_ms = k_uptime_get();
		LOG_INF("AWS IoT: connected (mutual TLS, %s session)",
			evt->data.persistent_session ? "persistent" : "clean");
		break;

	case AWS_IOT_EVT_DISCONNECTED: {
		uint32_t delay;

		is_connected = false;
		/* The reported state of a new session is never a repeat: the
		 * broker may have missed the last one, and a reconnection is
		 * exactly when the cloud needs to be told where the bike
		 * stands. */
		last_shadow_json[0] = '\0';
		/* A disconnection is a failure like any other and feeds the
		 * backoff. Otherwise a flapping link (unstable network while
		 * riding) would start a TLS handshake every ten seconds. */
		backoff_bump();
		delay = backoff_with_jitter();
		LOG_WRN("AWS IoT: disconnected — reconnecting in %u s "
			"(buffered trace: %d pts, alerts: %d)",
			delay, location_pending_samples(), alert_pending());
		(void)k_work_reschedule_for_queue(&cloud_wq, &connect_work,
						  K_SECONDS(delay));
		break;
	}

	case AWS_IOT_EVT_DATA_RECEIVED:
		LOG_INF("AWS IoT <- [%.*s]: %.*s",
			(int)evt->data.msg.topic.len, evt->data.msg.topic.str,
			(int)evt->data.msg.len, evt->data.msg.ptr);

		/* Device Shadow: desired (full document at boot) or delta. */
		if (evt->data.msg.topic.type_received ==
		    AWS_IOT_SHADOW_TOPIC_GET_ACCEPTED) {
			shadow_config_handle(evt->data.msg.ptr,
					     evt->data.msg.len, true);
			break;
		}
		if (evt->data.msg.topic.type_received ==
		    AWS_IOT_SHADOW_TOPIC_UPDATE_DELTA) {
			shadow_config_handle(evt->data.msg.ptr,
					     evt->data.msg.len, false);
			break;
		}

		/* Application command -> CH_CLOUD_CMD (state machine). */
		if (evt->data.msg.topic.len == sizeof(TOPIC_CMD) - 1 &&
		    memcmp(evt->data.msg.topic.str, TOPIC_CMD,
			   sizeof(TOPIC_CMD) - 1) == 0) {
			struct msg_cloud_cmd cmd;
			size_t len = MIN(evt->data.msg.len,
					 sizeof(cmd.json) - 1);

			memcpy(cmd.json, evt->data.msg.ptr, len);
			cmd.json[len] = '\0';

			(void)zbus_chan_pub(&CH_CLOUD_CMD, &cmd, K_MSEC(100));
		}
		break;

	case AWS_IOT_EVT_PUBACK:
		LOG_INF("AWS IoT: PUBACK (id %d)", evt->data.message_id);
		break;

	case AWS_IOT_EVT_PINGRESP:
		LOG_DBG("AWS IoT: PINGRESP");
		break;

	case AWS_IOT_EVT_ERROR:
		LOG_ERR("AWS IoT: error %d", evt->data.err);
		break;

	default:
		LOG_DBG("AWS IoT: event %d ignored", evt->type);
		break;
	}
}

int cloud_init(void)
{
	int err;
	static const struct mqtt_topic app_topics[] = {
		{
			.topic.utf8 = TOPIC_CMD,
			.topic.size = sizeof(TOPIC_CMD) - 1,
			.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		},
	};

	/* cJSON memory hooks (shadow document parsing). */
	cJSON_Init();

	/* Dedicated workqueue BEFORE any cloud work is scheduled. */
	k_work_queue_start(&cloud_wq, cloud_stack,
			   K_THREAD_STACK_SIZEOF(cloud_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&cloud_wq.thread, "ebike_cloud");
	(void)wdt_watch_queue(&cloud_wq, CONFIG_EBIKE_WDT_CLOUD_TIMEOUT_S,
			      "cloud workqueue");

	err = aws_iot_init(aws_iot_evt_handler);
	if (err) {
		LOG_ERR("aws_iot_init: %d", err);
		return err;
	}

	/* Subscribed to on connection; the array must stay valid afterwards. */
	err = aws_iot_application_topics_set(app_topics,
					     ARRAY_SIZE(app_topics));
	if (err) {
		LOG_ERR("aws_iot_application_topics_set: %d", err);
		return err;
	}

	return 0;
}

int cloud_connect(void)
{
	return k_work_reschedule_for_queue(&cloud_wq, &connect_work,
					   K_NO_WAIT) < 0 ? -EBUSY : 0;
}

bool cloud_is_connected(void)
{
	return is_connected;
}

static int publish_json(const char *topic, size_t topic_len, const char *json)
{
	int err;
	struct aws_iot_data msg = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic.str = topic,
		.topic.len = topic_len,
		.ptr = (char *)json,
		.len = strlen(json),
	};

	if (!is_connected) {
		return -ENOTCONN;
	}

	err = aws_iot_send(&msg);
	if (err) {
		LOG_ERR("aws_iot_send [%s]: %d", topic, err);
		return err;
	}

	LOG_INF("AWS IoT -> [%s]: %s", topic, json);

	return 0;
}

int cloud_publish_status(const char *json)
{
	return publish_json(TOPIC_STATUS, sizeof(TOPIC_STATUS) - 1, json);
}

int cloud_publish_telemetry(const char *json)
{
	return publish_json(TOPIC_TELEMETRY, sizeof(TOPIC_TELEMETRY) - 1, json);
}

/* Serialise and publish an alert. @p age_s is the number of seconds since it
 * was RAISED (0 for an immediate send, > 0 for one replayed after an outage).
 *
 * Without that age a deferred alert would be dated at its ARRIVAL time: the
 * notifier stamps it with the timestamp injected by the IoT rule, and the
 * e-mail would announce a theft "just now" that happened twenty minutes
 * earlier — exactly the information that matters when looking for a bike. */
static int alert_publish(const struct msg_alert *alert, int32_t age_s)
{
	char json[224];
	char pos[80] = "";
	char age[24] = "";

	/* Position fragment, common to all three causes. `fix_age_s` travels
	 * with it: without it, a position several minutes old (GNSS starved
	 * during an alarm) would read as the bike's current position. */
	if (alert->has_pos) {
		snprintf(pos, sizeof(pos),
			 ",\"lat\":%.6f,\"lon\":%.6f,\"acc_m\":%.0f"
			 ",\"fix_age_s\":%d",
			 alert->lat, alert->lon, (double)alert->acc_m,
			 alert->fix_age_s);
	}

	/* Field omitted in the nominal case, so nothing changes for immediate
	 * alerts, which are by far the most frequent. */
	if (age_s > 0) {
		snprintf(age, sizeof(age), ",\"age_s\":%d", age_s);
	}

	switch (alert->cause) {
	case ALERT_MOTION_WHILE_ARMED:
		snprintf(json, sizeof(json),
			 "{\"alert\":\"motion\",\"cause\":%d%s%s,"
			 "\"uptime_s\":%lld}",
			 alert->cause, pos, age, k_uptime_get() / 1000);
		break;

	case ALERT_GEOFENCE_EXIT:
	case ALERT_GEOFENCE_RETURN:
		snprintf(json, sizeof(json),
			 "{\"alert\":\"%s\",\"cause\":%d,\"dist_m\":%u%s%s"
			 ",\"uptime_s\":%lld}",
			 alert->cause == ALERT_GEOFENCE_EXIT ? "geofence"
							     : "geofence_in",
			 alert->cause, alert->dist_m, pos, age,
			 k_uptime_get() / 1000);
		break;

	default:
		return -EINVAL;
	}

	return publish_json(TOPIC_ALERT, sizeof(TOPIC_ALERT) - 1, json);
}

/* Drain the alert queue. ALWAYS runs on the cloud workqueue, never in the
 * publisher context (system workqueue for the geofence, main thread for the
 * anti-theft state). */
static void alert_flush(void)
{
	struct pending_alert pend;
	int64_t now = k_uptime_get();

	while (alert_peek(&pend)) {
		int32_t age_s = (int32_t)((now - pend.uptime_ms) / MSEC_PER_SEC);
		int err = alert_publish(&pend.alert, age_s);

		if (err == 0) {
			alert_drop();
			if (age_s > 0) {
				LOG_INF("Deferred alert replayed (raised %d s "
					"ago)", age_s);
			}
			continue;
		}

		/* Unknown cause: nothing sendable. Drop it anyway, otherwise it
		 * would block the queue forever by failing on every attempt. */
		if (err == -EINVAL) {
			alert_drop();
			LOG_WRN("Alert with unknown cause (%d) discarded",
				pend.alert.cause);
			continue;
		}

		/* Link unavailable: it STAYS in the queue, in place and with its
		 * original time, and leaves when the network returns. */
		LOG_WRN("%d alert(s) pending — cloud link unavailable (%d)",
			alert_pending(), err);
		return;
	}
}

static void alert_work_fn(struct k_work *work)
{
	alert_flush();
}

/* ---------- Aggregated telemetry builder ----------
 * Consumes the zbus channels (last value published by each producer) plus the
 * batched GNSS trace, and publishes at the rate set by the state machine.
 * Runs on the cloud workqueue, so static buffers are safe (single context). */

/* Publish one trace batch. `age_s` is the age of the LAST point of the batch
 * at send time.
 *
 * WHY THAT AGE. On the cloud side every point is re-timestamped relative to
 * the ARRIVAL time of the message, since the firmware only sends uptimes. As
 * long as the batch leaves fresh, "last point = now" holds. It collapses as
 * soon as a backlog is replayed: 20 minutes of trace accumulated offline would
 * pile up at the moment of the catch-up, and successive batches would all land
 * on the same instant. `age` gives the cloud the real anchor point. The field
 * is omitted when zero, so nothing changes for ordinary messages.
 */
static int publish_batch(const struct location_sample *pts, int n, int32_t age_s)
{
	/* 16 points of about 75 bytes plus the header. The margin is generous
	 * on purpose: truncation here would cut a catch-up batch short. */
	static char batch[1536];
	size_t off;

	off = snprintf(batch, sizeof(batch), "{\"n\":%d,", n);
	if (age_s > 0) {
		off += snprintf(batch + off, sizeof(batch) - off,
				"\"age\":%d,", age_s);
	}
	off += snprintf(batch + off, sizeof(batch) - off, "\"pts\":[");

	for (int i = 0; i < n; i++) {
		int w;

		/* `spd` is OMITTED when the speed failed the plausibility
		 * filter: a clean hole beats a wrong number — the console then
		 * breaks the curve instead of drawing a spike that never
		 * happened. */
		if (pts[i].spd_valid) {
			w = snprintf(batch + off, sizeof(batch) - off,
				"%s{\"t\":%u,\"lat\":%.6f,\"lon\":%.6f,"
				"\"spd\":%.1f,\"acc\":%.0f}",
				i ? "," : "", pts[i].t_s,
				pts[i].lat, pts[i].lon,
				(double)pts[i].spd_kmh,
				(double)pts[i].acc_m);
		} else {
			w = snprintf(batch + off, sizeof(batch) - off,
				"%s{\"t\":%u,\"lat\":%.6f,\"lon\":%.6f,"
				"\"acc\":%.0f}",
				i ? "," : "", pts[i].t_s,
				pts[i].lat, pts[i].lon,
				(double)pts[i].acc_m);
		}

		if (w < 0 || (size_t)w >= sizeof(batch) - off) {
			batch[off] = '\0';
			LOG_WRN("Batch truncated at %d points", i);
			n = i;
			break;
		}
		off += w;
	}

	snprintf(batch + off, sizeof(batch) - off, "]}");

	if (n == 0) {
		return 0;
	}

	return cloud_publish_telemetry(batch) == 0 ? n : -EIO;
}

/* Drain the trace buffer, bounded to a few batches per cycle.
 *
 * The bound matters: replaying 20 minutes of backlog in one go would hold the
 * radio for tens of seconds and starve the GNSS. Spread it out instead.
 */
static void publish_trace(int64_t now)
{
	static struct location_sample pts[16];

	for (int b = 0; b < CONFIG_EBIKE_BACKLOG_BATCHES_PER_CYCLE; b++) {
		int32_t age_s;
		int sent;
		int n = location_peek_samples(pts, ARRAY_SIZE(pts));

		if (n == 0) {
			return;
		}

		age_s = (int32_t)(now / MSEC_PER_SEC) - (int32_t)pts[n - 1].t_s;
		sent = publish_batch(pts, n, age_s > 0 ? age_s : 0);
		if (sent <= 0) {
			/* Send refused: the points stay in the buffer and leave
			 * on the next cycle. That is the whole point of the
			 * peek/drop split — a fragile reconnection no longer
			 * costs the trace it was carrying. */
			LOG_WRN("Batch not published — %d points kept in buffer",
				n);
			return;
		}

		location_drop_samples(sent);
	}

	if (location_pending_samples() > 0) {
		LOG_INF("Trace catch-up: %d points still buffered",
			location_pending_samples());
	}
}

static void publish_work_fn(struct k_work *work)
{
	static char json[576];
	struct msg_speed spd = {0};
	struct msg_motion mot = {0};
	uint32_t interval = (uint32_t)atomic_get(&publish_interval_s);
	int64_t now = k_uptime_get();

	/* Rescheduled first: a failed publication must not kill the cycle. */
	if (interval > 0) {
		(void)k_work_reschedule_for_queue(&cloud_wq, &publish_work,
						  K_SECONDS(interval));
	}

	if (!is_connected) {
		return;
	}

	last_publish_ms = now;

	/* Pending alerts first: they are the most urgent messages in the
	 * system and go before telemetry. (Safety net — enqueuing an alert
	 * already wakes alert_work.) */
	alert_flush();

	/* Shadow report held back by the debounce leaves grouped here, in the
	 * same radio wake-up window as the status. */
	if (shadow_report_due) {
		(void)shadow_report_send(&shadow_cfg, sm_state_str);
	}

	/* Re-read the commands while the connection is up. */
	shadow_get_if_due(now);

	/* Refresh the RSRP (also logs the signal quality). */
	(void)connectivity_log_signal_quality();

	(void)zbus_chan_read(&CH_SPEED, &spd, K_MSEC(100));
	(void)zbus_chan_read(&CH_MOTION, &mot, K_MSEC(100));

	/* The status carries more than the live values, because everything here
	 * is archived and a past session has to be readable without a console:
	 *   sm_state/armed  the shadow has no history, and without them past
	 *                   states can only be guessed from the publish rate;
	 *   ttff_s + agnss  the pair that explains a slow first fix;
	 *   gnss_*          RF starvation and priority mode;
	 *   rej_* / sacc    plausibility filter activity and the worst speed
	 *                   uncertainty seen, used to calibrate the filter;
	 *   qlen            what is still buffered offline;
	 *   reset/crash/boot  why the device restarted. */
	size_t len = snprintf(json, sizeof(json),
		"{\"uptime_s\":%lld,\"rsrp_dbm\":%d"
		",\"sm_state\":\"%s\",\"armed\":%s"
		",\"sats\":%u,\"sats_used\":%u,\"gnss_starved\":%u,\"gnss_prio\":%u"
		",\"fix_age_s\":%d,\"ttff_s\":%d,\"agnss\":\"%s\""
		",\"rej_fix\":%u,\"rej_spd\":%u,\"sacc\":%.1f"
		",\"spd_wheel\":%.1f,\"revs\":%u"
		",\"acc\":[%.2f,%.2f,%.2f]"
		",\"qlen\":%d"
		",\"reset\":\"%s\",\"crash\":\"%s\",\"boot\":\"%s\"}",
		k_uptime_get() / 1000,
		connectivity_last_rsrp_dbm(),
		sm_state_str, sm_armed ? "true" : "false",
		location_sats_tracked(), location_sats_used(),
		location_starved_count(), location_prio_mode() ? 1U : 0U,
		location_fix_age_s(), location_ttff_s(),
		assistance_status_str(),
		location_rejected_fixes(), location_rejected_speeds(),
		(double)location_worst_speed_acc_kmh(),
		(double)spd.kmh, spd.revs,
		(double)mot.acc_ms2[0], (double)mot.acc_ms2[1],
		(double)mot.acc_ms2[2],
		location_pending_samples(),
		diag_reset_cause_str(), diag_crash_str(), diag_boot_str());

	if (len < sizeof(json)) {
		(void)cloud_publish_status(json);
	} else {
		LOG_WRN("Status truncated (%zu bytes) — not published", len);
	}

	/* Position trace, backlog included. */
	publish_trace(now);
}

void cloud_set_publish_interval(uint32_t seconds)
{
	if (seconds == 0) {
		return;
	}

	atomic_set(&publish_interval_s, (atomic_val_t)seconds);
	LOG_INF("Publication rate: %u s", seconds);

	/* Publish immediately on a state change, then restart the cycle at the
	 * new rate — UNLESS the previous publication is too recent: a burst of
	 * transitions would hold the radio and starve the GNSS. In that case it
	 * is postponed just enough to respect the debounce; the information is
	 * not lost, only delayed. */
	k_timeout_t delay = K_NO_WAIT;

	if (CONFIG_EBIKE_TRANSITION_DEBOUNCE_S > 0 && last_publish_ms >= 0) {
		int64_t since = k_uptime_get() - last_publish_ms;
		int64_t debounce =
			(int64_t)CONFIG_EBIKE_TRANSITION_DEBOUNCE_S * MSEC_PER_SEC;

		if (since < debounce) {
			delay = K_MSEC(debounce - since);
			LOG_DBG("Transition publication deferred by %lld ms",
				debounce - since);
		}
	}

	(void)k_work_reschedule_for_queue(&cloud_wq, &publish_work, delay);
}

/* ---------- Alerts (CH_ALERT -> immediate MQTT) ---------- */

static void alert_listener_cb(const struct zbus_channel *chan)
{
	const struct msg_alert *alert = zbus_chan_const_msg(chan);

	/* zbus listener: runs in the publisher's context — the SYSTEM
	 * workqueue for the geofence (listener on CH_GNSS_FIX), the main thread
	 * for the anti-theft state. Timestamp and enqueue, nothing more: the
	 * MQTT/TLS send happens on the cloud workqueue, woken immediately. The
	 * alert stays immediate without blocking GNSS frame processing. */
	alert_enqueue(alert, k_uptime_get());
	(void)k_work_submit_to_queue(&cloud_wq, &alert_work);
}

ZBUS_LISTENER_DEFINE(cloud_alert_lis, alert_listener_cb);
ZBUS_CHAN_ADD_OBS(CH_ALERT, cloud_alert_lis, 3);
