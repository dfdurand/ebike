/*
 * eBike tracker — GNSS positioning through nrf_modem_gnss.
 *
 * Periodic fixes coexisting with the LTE link. When LTE leaves no RF window
 * to the receiver (repeated NOT_ENOUGH_WINDOW_TIME flags), GNSS priority mode
 * is enabled temporarily until the next fix.
 */

#ifndef EBIKE_LOCATION_H_
#define EBIKE_LOCATION_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Last valid GNSS fix. */
struct location_fix {
	double lat;        /* degrees */
	double lon;        /* degrees */
	float alt_m;
	float accuracy_m;  /* horizontal uncertainty (m) */
	float speed_kmh;   /* ground speed, only meaningful when spd_valid */
	bool spd_valid;    /* speed passed the plausibility filter */
	float heading_deg;
	uint8_t sats_used; /* satellites used in the fix */
	int64_t age_ms;    /* age of the fix, computed on read */
};

/**
 * @brief Enable GNSS and start periodic fixes.
 *
 * Call once the modem is initialised (the system mode must include GNSS:
 * CONFIG_LTE_NETWORK_MODE_LTE_M_GPS). Works before or after the LTE attach.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int location_start(void);

/**
 * @brief Fetch the last valid fix.
 *
 * @param[out] fix Filled when a valid fix exists.
 * @retval true when a valid fix is available.
 */
bool location_get_fix(struct location_fix *fix);

/** Satellites currently tracked (diagnostic, meaningful before a fix). */
uint8_t location_sats_tracked(void);

/**
 * @brief Satellites used in the last valid fix (0 until the first one).
 *
 * The gap with location_sats_tracked() reveals RF starvation: satellites are
 * seen but there is never enough window to resolve a fix.
 */
uint8_t location_sats_used(void);

/**
 * @brief Consecutive PVT epochs with insufficient RF window (LTE too busy).
 *
 * Reset on every valid fix. A rising value means the GNSS is starved.
 */
uint32_t location_starved_count(void);

/** Whether GNSS priority mode (anti-starvation) is currently active. */
bool location_prio_mode(void);

/** Age of the last valid fix in seconds, -1 when there has been none. */
int32_t location_fix_age_s(void);

/**
 * @brief Time to first fix: seconds between boot and the first valid fix.
 *
 * -1 until a fix is obtained. Published in the status as `ttff_s`, where it
 * answers the only question that matters after a slow start: did the A-GNSS
 * assistance actually help, or did the receiver search on its own?
 */
int32_t location_ttff_s(void);

/**
 * @brief Fixes discarded by the plausibility filter since boot.
 *
 * Solutions whose horizontal uncertainty exceeds EBIKE_GNSS_MAX_ACC_M: no
 * trace point, no geofence evaluation, no "last known fix" update.
 */
uint32_t location_rejected_fixes(void);

/**
 * @brief Speeds discarded by the plausibility filter since boot.
 *
 * The position of such a fix is kept; only its speed is marked unknown.
 */
uint32_t location_rejected_speeds(void);

/**
 * @brief Worst speed uncertainty (`speed_accuracy`) seen since boot, in km/h.
 *
 * -1 when no PVT has produced a valid speed yet. Published so that
 * CONFIG_EBIKE_GNSS_MAX_SPEED_ACC_KMH can be calibrated from real data
 * instead of being guessed.
 */
float location_worst_speed_acc_kmh(void);

/**
 * Timestamped position sample of the trace.
 *
 * Fields are ordered from the strictest alignment to the loosest, which makes
 * the structure exactly 32 bytes with no padding. That matters: it is
 * replicated CONFIG_EBIKE_TRACE_RING_SIZE times, and the naive order costs
 * 40 bytes per point, about 2 KB of RAM in pure padding at the default size.
 */
struct location_sample {
	double lat;
	double lon;
	uint32_t t_s;   /* uptime (s) of the sampled fix */
	float spd_kmh;
	float acc_m;
	bool spd_valid; /* false => speed unknown, `spd` omitted from the JSON */
};

/**
 * @brief Read the oldest samples WITHOUT removing them from the buffer.
 *
 * Samples accumulate every CONFIG_EBIKE_TELEMETRY_SAMPLE_S seconds, only when
 * a new fix has arrived. The buffer keeps the last
 * CONFIG_EBIKE_TRACE_RING_SIZE points and overwrites the oldest when full.
 *
 * This is the offline buffer: while the MQTT link is down nothing is drained
 * and the trace piles up here, so the caller may find far more than fits in a
 * single message — hence the catch-up in successive batches once the network
 * is back.
 *
 * The read is deliberately non-destructive: points are only removed by
 * location_drop_samples(), once a publication has actually been accepted.
 * Without that split, a send failing right after a fragile reconnection would
 * lose the very points the buffer exists to protect.
 *
 * @param[out] out Destination array.
 * @param max Capacity of the array.
 * @return Number of samples copied.
 */
int location_peek_samples(struct location_sample *out, int max);

/**
 * @brief Set the trace sampling period.
 *
 * Called by the state machine on state entry: fine while RIDING and ALARM,
 * reduced while PARKED.
 *
 * This affects the TRACE ONLY. The last known fix, CH_GNSS_FIX and therefore
 * the geofence keep being fed by every valid fix, so slowing the sampling down
 * never blunts exit detection or the position attached to alerts.
 *
 * @param seconds Period between two samples (> 0). Takes effect immediately.
 */
void location_set_sample_interval(uint32_t seconds);

/**
 * @brief Remove the @p n oldest samples from the buffer.
 *
 * Call after a successful publication with the number of points actually
 * sent, typically the value returned by location_peek_samples().
 *
 * @param n Number of samples to drop (clamped to the buffer contents).
 */
void location_drop_samples(int n);

/**
 * @brief Number of samples waiting to be published.
 *
 * Lets the cloud module know whether there is still a backlog to catch up on
 * after an outage, and reschedule a cycle without waiting.
 */
int location_pending_samples(void);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_LOCATION_H_ */
