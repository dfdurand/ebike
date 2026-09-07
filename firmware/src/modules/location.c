/*
 * eBike tracker — GNSS module implementation.
 *
 * The GNSS event handler runs in interrupt context: it reads the PVT frame and
 * defers all processing to the system workqueue.
 */

#include "location.h"
#include "events/channels.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>

#include <string.h>

#if defined(CONFIG_EBIKE_AGNSS)
#include "assistance.h"
#endif

LOG_MODULE_REGISTER(location, CONFIG_EBIKE_LOG_LEVEL);

/* Beyond this many consecutive PVT epochs without a usable RF window, GNSS
 * priority mode is forced until the next fix. */
#define WINDOW_STARVED_THRESHOLD 10

/* Progress logs (tracked satellites) at most every N seconds. */
#define TRACKING_LOG_PERIOD_MS (30 * MSEC_PER_SEC)

static struct nrf_modem_gnss_pvt_data_frame pvt_isr; /* filled in ISR */
static void pvt_work_fn(struct k_work *work);
static K_WORK_DEFINE(pvt_work, pvt_work_fn);

#if defined(CONFIG_EBIKE_AGNSS)
static struct nrf_modem_gnss_agnss_data_frame agnss_isr; /* filled in ISR */
#endif

static struct k_spinlock fix_lock;
static struct location_fix last_fix;
static int64_t last_fix_uptime = -1;

/* Trace: position samples accumulated between two SUCCESSFUL publications.
 * Sized to absorb a network outage while riding — offline, the cloud module
 * drains nothing and everything piles up here. The default 240 slots hold
 * 20 minutes of trace while riding and 4 hours while parked. */
#define SAMPLE_RING_SIZE CONFIG_EBIKE_TRACE_RING_SIZE

static struct location_sample sample_ring[SAMPLE_RING_SIZE];
static int sample_head;  /* next write slot */
static int sample_count;
static int64_t last_sampled_fix_uptime = -1;
static uint32_t sample_dropped; /* points overwritten for lack of room */

static void sample_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, sample_work_fn);

/* Current sampling rate, driven by the state machine: fine in RIDING/ALARM,
 * reduced in PARKED. Atomic: written from main, read from the workqueue. */
static atomic_t sample_interval_s = ATOMIC_INIT(CONFIG_EBIKE_TELEMETRY_SAMPLE_S);

/* Tracked satellites: the BEST count over a sliding window, not the value of
 * the latest PVT frame.
 *
 * In continuous navigation the modem emits one frame per second, including for
 * epochs where the receiver got no observation window at all — their SV list
 * is then empty. Reporting the current frame therefore showed "0 satellites"
 * in the middle of perfectly good reception, which reads as a receiver
 * failure that never happened. */
#define SATS_WINDOW_MS (30 * MSEC_PER_SEC)

static volatile uint8_t sats_tracked;   /* exposed value: max of both windows */
static uint8_t sats_window_best;        /* current window accumulator */
static uint8_t sats_window_prev;        /* previous window, kept on purpose */
static int64_t sats_window_start;
static volatile uint8_t sats_used_live; /* satellites of the LAST valid fix */
static uint32_t starved_count;
static bool prio_mode_active;
static int64_t last_tracking_log;

/* TTFF: uptime of the very first valid fix (-1 until it lands). */
static int64_t first_fix_uptime = -1;

/* Plausibility filter counters, published in the status. */
static uint32_t rejected_fixes;
static uint32_t rejected_speeds;
static float worst_speed_acc_kmh = -1.0f;

static void sample_work_fn(struct k_work *work)
{
	uint32_t dropped_before = sample_dropped;

	K_SPINLOCK(&fix_lock) {
		/* Only sample when a new fix arrived since the previous one, so
		 * a bike standing under a roof does not fill the buffer with
		 * duplicates. */
		if (last_fix_uptime >= 0 &&
		    last_fix_uptime != last_sampled_fix_uptime) {
			sample_ring[sample_head] = (struct location_sample){
				.t_s = (uint32_t)(last_fix_uptime / 1000),
				.lat = last_fix.lat,
				.lon = last_fix.lon,
				.spd_kmh = last_fix.speed_kmh,
				.spd_valid = last_fix.spd_valid,
				.acc_m = last_fix.accuracy_m,
			};
			sample_head = (sample_head + 1) % SAMPLE_RING_SIZE;
			if (sample_count < SAMPLE_RING_SIZE) {
				sample_count++;
			} else {
				/* Buffer full: the oldest point was just
				 * overwritten. The outage now outlasts the
				 * buffer depth and the trace starts to be
				 * lost for real. */
				sample_dropped++;
			}
			last_sampled_fix_uptime = last_fix_uptime;
		}
	}

	/* Outside the lock, and only on the first overflow: past that point the
	 * trace is lost one sample at a time, no need to repeat it. */
	if (sample_dropped != dropped_before && dropped_before == 0) {
		LOG_WRN("Trace buffer full (%d points, %lld s) — oldest are "
			"being overwritten: prolonged network outage",
			SAMPLE_RING_SIZE,
			(int64_t)SAMPLE_RING_SIZE * atomic_get(&sample_interval_s));
	}

	(void)k_work_schedule(&sample_work,
			      K_SECONDS(atomic_get(&sample_interval_s)));
}

void location_set_sample_interval(uint32_t seconds)
{
	if (seconds == 0 ||
	    (uint32_t)atomic_get(&sample_interval_s) == seconds) {
		return;
	}

	atomic_set(&sample_interval_s, (atomic_val_t)seconds);
	LOG_INF("Trace: one sample every %u s", seconds);

	/* Reschedule right away. Without this, resuming movement would wait out
	 * the whole parked period before returning to the fine rate — up to a
	 * minute of riding recorded at parking resolution. */
	(void)k_work_reschedule(&sample_work, K_SECONDS(seconds));
}

int location_peek_samples(struct location_sample *out, int max)
{
	int n = 0;

	K_SPINLOCK(&fix_lock) {
		int idx = (sample_head - sample_count + SAMPLE_RING_SIZE) %
			  SAMPLE_RING_SIZE;

		while (n < sample_count && n < max) {
			out[n++] = sample_ring[idx];
			idx = (idx + 1) % SAMPLE_RING_SIZE;
		}
	}

	return n;
}

void location_drop_samples(int n)
{
	K_SPINLOCK(&fix_lock) {
		/* The buffer may have wrapped between the peek and the drop
		 * (a sample added while publishing), so never remove more than
		 * it currently holds. */
		sample_count -= MIN(n, sample_count);
	}
}

static uint8_t count_sats(const struct nrf_modem_gnss_pvt_data_frame *pvt,
			  uint8_t *used_in_fix)
{
	uint8_t tracked = 0;
	uint8_t used = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt->sv[i].sv == 0) {
			continue;
		}
		tracked++;
		if (pvt->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
			used++;
		}
	}

	if (used_in_fix != NULL) {
		*used_in_fix = used;
	}

	return tracked;
}

/*
 * Speed plausibility filter.
 *
 * The modem's instantaneous Doppler speed is noisy at low speed: a point can
 * be well located (9 m accuracy) and still report 75 km/h where the speed
 * derived from successive positions is 16 km/h. A filter based on POSITION
 * accuracy does not catch those — the position is fine, the speed is not.
 *
 * Three guards, from the safest to the finest:
 *  1. the modem VELOCITY_VALID flag — without it the value is meaningless;
 *  2. a km/h ceiling — deterministic, nothing to guess;
 *  3. `speed_accuracy` (the 1-sigma figure the modem provides) — the right
 *     statistical criterion, but DISABLED by default: calibrating it blind
 *     risks throwing away nearly every speed. The worst case observed is
 *     published instead, so the threshold can be set from real data.
 *
 * The POSITION is always kept: it is what carries the trace.
 */
static bool speed_is_plausible(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	float acc_kmh = pvt->speed_accuracy * 3.6f;

	if (!(pvt->flags & NRF_MODEM_GNSS_PVT_FLAG_VELOCITY_VALID)) {
		return false;
	}

	/* Track the worst case even when the 1-sigma threshold is disabled:
	 * this is the measurement that allows enabling it later. */
	if (acc_kmh > worst_speed_acc_kmh) {
		worst_speed_acc_kmh = acc_kmh;
	}

	if (pvt->speed * 3.6f > (float)CONFIG_EBIKE_GNSS_MAX_SPEED_KMH) {
		return false;
	}

	if (CONFIG_EBIKE_GNSS_MAX_SPEED_ACC_KMH > 0 &&
	    acc_kmh > (float)CONFIG_EBIKE_GNSS_MAX_SPEED_ACC_KMH) {
		return false;
	}

	return true;
}

static void pvt_work_fn(struct k_work *work)
{
	struct nrf_modem_gnss_pvt_data_frame pvt;
	uint8_t used = 0;
	uint8_t tracked;
	bool spd_ok;
	int64_t pvt_now = k_uptime_get();

	/* Local copy: pvt_isr can be rewritten by the next ISR. */
	unsigned int key = irq_lock();
	pvt = pvt_isr;
	irq_unlock(key);

	tracked = count_sats(&pvt, &used);

	/* Two buckets rather than one: with a single window cleared in one go,
	 * a status published right after the switch would only see the current
	 * frame — zero if that frame is blind, which is exactly what this
	 * avoids. Keeping the previous window guarantees 30 to 60 s of
	 * history at all times. */
	if (pvt_now - sats_window_start >= SATS_WINDOW_MS) {
		sats_window_start = pvt_now;
		sats_window_prev = sats_window_best;
		sats_window_best = tracked;
	} else if (tracked > sats_window_best) {
		sats_window_best = tracked;
	}
	sats_tracked = MAX(sats_window_best, sats_window_prev);

	if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		/* Clearly bad solution: drop it whole rather than pollute the
		 * trace, the geofence and the last known fix. In continuous
		 * navigation (one fix per second) that costs nothing. The
		 * threshold is deliberately wide — reject garbage, do not
		 * filter on quality. */
		if (pvt.accuracy > (float)CONFIG_EBIKE_GNSS_MAX_ACC_M) {
			rejected_fixes++;
			LOG_DBG("Fix rejected: accuracy %.0f m > %d m",
				(double)pvt.accuracy, CONFIG_EBIKE_GNSS_MAX_ACC_M);
			return;
		}

		spd_ok = speed_is_plausible(&pvt);
		if (!spd_ok) {
			rejected_speeds++;
			LOG_DBG("Speed rejected: %.1f km/h (+/-%.1f km/h)",
				(double)(pvt.speed * 3.6f),
				(double)(pvt.speed_accuracy * 3.6f));
		}

		/* Updated ONLY here. On a blind epoch or a rejected fix, keep
		 * the value of the last accepted fix: otherwise `sats_used`
		 * would drop to 0 while `fix_age_s` says two seconds, which
		 * reads as a reception loss that did not happen. */
		sats_used_live = used;

		K_SPINLOCK(&fix_lock) {
			last_fix.lat = pvt.latitude;
			last_fix.lon = pvt.longitude;
			last_fix.alt_m = pvt.altitude;
			last_fix.accuracy_m = pvt.accuracy;
			last_fix.speed_kmh = pvt.speed * 3.6f;
			last_fix.spd_valid = spd_ok;
			last_fix.heading_deg = pvt.heading;
			last_fix.sats_used = used;
			last_fix_uptime = pvt_now;
			if (first_fix_uptime < 0) {
				first_fix_uptime = pvt_now;
			}
		}

		starved_count = 0;

		/* Publish for the telemetry builder and the geofence. */
		const struct msg_gnss_fix msg = {
			.lat = pvt.latitude,
			.lon = pvt.longitude,
			.acc_m = pvt.accuracy,
			.spd_kmh = pvt.speed * 3.6f,
			.sats = used,
			.valid = true,
		};

		(void)zbus_chan_pub(&CH_GNSS_FIX, &msg, K_MSEC(100));

		/* One fix per second in continuous navigation, so the log is
		 * throttled; the detail goes out with the telemetry anyway. */
		static int64_t last_fix_log;
		int64_t now_fix = k_uptime_get();

		if (now_fix - last_fix_log >= TRACKING_LOG_PERIOD_MS) {
			last_fix_log = now_fix;
			LOG_INF("GNSS fix: %.6f, %.6f (+/-%.0f m, alt %.0f m, "
				"%.1f km/h, %u sats)",
				pvt.latitude, pvt.longitude, (double)pvt.accuracy,
				(double)pvt.altitude, (double)(pvt.speed * 3.6f),
				used);
		}

		/* TTFF is logged once, when the first fix lands. The value also
		 * goes into the status, where it tells whether A-GNSS helped. */
		static bool ttff_logged;

		if (!ttff_logged) {
			ttff_logged = true;
			LOG_INF("TTFF: first fix %d s after boot",
				(int)(first_fix_uptime / 1000));
		}

		/* First fix obtained: priority mode is no longer needed. */
		if (prio_mode_active) {
			(void)nrf_modem_gnss_prio_mode_disable();
			prio_mode_active = false;
			LOG_INF("GNSS priority mode disabled (fix obtained)");
		}

		return;
	}

	/* No fix: watch the LTE/GNSS coexistence. */
	if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
		starved_count++;

		if (starved_count == WINDOW_STARVED_THRESHOLD && !prio_mode_active) {
			LOG_WRN("GNSS starved by LTE (%u PVT without RF window) "
				"— enabling priority mode until the next fix",
				starved_count);
			if (nrf_modem_gnss_prio_mode_enable() == 0) {
				prio_mode_active = true;
			}
		}
	}

	/* This log only fires on a fix-less epoch, which in continuous
	 * navigation is almost always a blind one. Reporting the count of THAT
	 * frame showed a permanent "0 satellites" between two five-satellite
	 * fixes. Report the best count of the window instead, plus the age of
	 * the last fix to remove any ambiguity. */
	if (pvt_now - last_tracking_log >= TRACKING_LOG_PERIOD_MS) {
		int32_t age = location_fix_age_s();
		const char *starved =
			(pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME)
				? ", insufficient RF window" : "";

		last_tracking_log = pvt_now;

		if (age < 0) {
			LOG_INF("GNSS: searching for a fix (%u satellites "
				"tracked recently, no fix since boot%s)",
				sats_tracked, starved);
		} else {
			LOG_INF("GNSS: searching for a fix (%u satellites "
				"tracked recently, last fix %d s ago%s)",
				sats_tracked, age, starved);
		}
	}
}

/* ISR context — minimal: read the frame and defer. */
static void gnss_event_handler(int event)
{
	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
	case NRF_MODEM_GNSS_EVT_FIX:
		if (nrf_modem_gnss_read(&pvt_isr, sizeof(pvt_isr),
					NRF_MODEM_GNSS_DATA_PVT) == 0) {
			k_work_submit(&pvt_work);
		}
		break;

	case NRF_MODEM_GNSS_EVT_BLOCKED:
		/* GNSS blocked by LTE activity — already counted through the
		 * PVT flags; nothing to do in ISR context. */
		break;

#if defined(CONFIG_EBIKE_AGNSS)
	case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
		/* The modem is asking for assistance data: read the frame and
		 * defer. assistance_request() is ISR-safe and asynchronous —
		 * the blocking network fetch runs on its own workqueue. */
		if (nrf_modem_gnss_read(&agnss_isr, sizeof(agnss_isr),
					NRF_MODEM_GNSS_DATA_AGNSS_REQ) == 0) {
			(void)assistance_request(&agnss_isr);
		}
		break;
#endif

	default:
		break;
	}
}

int location_start(void)
{
	int err;

#if defined(CONFIG_EBIKE_AGNSS)
	/* A-GNSS assistance, ready to answer modem requests. Non-blocking and
	 * opens no connection here. A failure is not fatal: GNSS still works
	 * autonomously, just with a longer TTFF. */
	err = assistance_init();
	if (err) {
		LOG_WRN("A-GNSS unavailable (%d) — autonomous GNSS search", err);
	}
#endif

	/* lte_lc owns the system mode (LTE-M + GNSS); only the GNSS side of
	 * the modem still needs activating. */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("GNSS activation failed: %d", err);
		return err;
	}

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_gnss_event_handler_set: %d", err);
		return err;
	}

	/* Multiple hot starts: the modem keeps ephemerides and almanac in RAM
	 * between periodic fixes. */
	err = nrf_modem_gnss_use_case_set(NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START);
	if (err) {
		LOG_ERR("nrf_modem_gnss_use_case_set: %d", err);
		return err;
	}

	err = nrf_modem_gnss_fix_retry_set(CONFIG_EBIKE_GNSS_FIX_RETRY);
	if (err) {
		LOG_ERR("nrf_modem_gnss_fix_retry_set: %d", err);
		return err;
	}

	err = nrf_modem_gnss_fix_interval_set(CONFIG_EBIKE_GNSS_FIX_INTERVAL);
	if (err) {
		LOG_ERR("nrf_modem_gnss_fix_interval_set: %d", err);
		return err;
	}

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("nrf_modem_gnss_start: %d", err);
		return err;
	}

#if CONFIG_EBIKE_GNSS_FIX_INTERVAL == 1
	LOG_INF("GNSS started (continuous navigation, sampling %d s riding / "
		"%d s parked)", CONFIG_EBIKE_TELEMETRY_SAMPLE_S,
		CONFIG_EBIKE_TELEMETRY_SAMPLE_PARKED_S);
#else
	LOG_INF("GNSS started (periodic fix: %d s, retry %d s)",
		CONFIG_EBIKE_GNSS_FIX_INTERVAL, CONFIG_EBIKE_GNSS_FIX_RETRY);
#endif

	/* Start trace sampling. The state machine adjusts the rate on its
	 * first state entry. */
	(void)k_work_schedule(&sample_work,
			      K_SECONDS(atomic_get(&sample_interval_s)));

	return 0;
}

bool location_get_fix(struct location_fix *fix)
{
	bool valid = false;

	K_SPINLOCK(&fix_lock) {
		if (last_fix_uptime >= 0) {
			*fix = last_fix;
			fix->age_ms = k_uptime_get() - last_fix_uptime;
			valid = true;
		}
	}

	return valid;
}

uint8_t location_sats_tracked(void)
{
	return sats_tracked;
}

uint8_t location_sats_used(void)
{
	return sats_used_live;
}

uint32_t location_starved_count(void)
{
	return starved_count;
}

bool location_prio_mode(void)
{
	return prio_mode_active;
}

int32_t location_fix_age_s(void)
{
	int32_t age = -1;

	K_SPINLOCK(&fix_lock) {
		if (last_fix_uptime >= 0) {
			age = (int32_t)((k_uptime_get() - last_fix_uptime) / 1000);
		}
	}

	return age;
}

int32_t location_ttff_s(void)
{
	int32_t ttff = -1;

	K_SPINLOCK(&fix_lock) {
		if (first_fix_uptime >= 0) {
			ttff = (int32_t)(first_fix_uptime / 1000);
		}
	}

	return ttff;
}

uint32_t location_rejected_fixes(void)
{
	return rejected_fixes;
}

uint32_t location_rejected_speeds(void)
{
	return rejected_speeds;
}

float location_worst_speed_acc_kmh(void)
{
	return worst_speed_acc_kmh;
}

int location_pending_samples(void)
{
	int n;

	K_SPINLOCK(&fix_lock) {
		n = sample_count;
	}

	return n;
}
