/*
 * eBike tracker — boot diagnostics: reset cause and crash capture.
 *
 * Reads the RESETREAS register through hwinfo at startup and clears it, so
 * every boot reports ITS own cause. The cause is attached to the MQTT status
 * (`reset` field), which makes repeated field reboots traceable remotely,
 * without a cable: supply brownout, software fatal error or pin/POR reset.
 */

#ifndef EBIKE_DIAG_H_
#define EBIKE_DIAG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read and clear the last reset cause (hwinfo).
 *
 * Call once, at the very start of main(), before any deliberate reset.
 *
 * @retval 0 on success, negative errno when hwinfo is unavailable.
 */
int diag_init(void);

/** @brief Raw reset cause mask (hwinfo RESET_* bits). */
uint32_t diag_reset_cause(void);

/**
 * @brief Reset cause as a short string for the telemetry JSON.
 *
 * '+'-separated tokens ("por", "pin", "brownout", "software", "watchdog",
 * "lockup"), "0xNN" when unknown, "n/a" when hwinfo failed.
 */
const char *diag_reset_cause_str(void);

/**
 * @brief Details of the fatal error that caused the current boot.
 *
 * Captured by the fatal handler in retained RAM and read back by diag_init().
 * Format: "r<reason> pc=0x.. lr=0x.. <thread>", or an empty string when this
 * boot does not follow a crash. Attached to the status as `crash`, so the next
 * ride reports the exact fault — reason, faulting instruction and guilty
 * thread — with no cable attached.
 */
const char *diag_crash_str(void);

/**
 * @brief Does the current boot follow a WARM reset (RAM retained)?
 *
 * true:  SRAM survived, so this is a warm reset — fatal handler, pin,
 *        watchdog or the secure world. It is NOT a power loss.
 * false: SRAM was lost, so this is a power-on (battery plugged in, hard
 *        brownout).
 *
 * This complements diag_reset_cause_str(), which is often silent on nRF91
 * because TF-M consumes RESETREAS before the non-secure application runs, and
 * diag_crash_str(), which only knows about application fatal errors:
 *   warm + crash set   -> application fatal error
 *   warm + crash empty -> watchdog or secure-side fault
 *   cold               -> power supply
 */
bool diag_boot_is_warm(void);

/**
 * @brief Boots since the last power-on (1 on the first).
 *
 * A counter that climbs while diag_crash_str() stays empty points at the
 * power supply or the secure world, never at the application.
 */
uint32_t diag_boot_count(void);

/**
 * @brief Boot marker as a short string for the telemetry JSON.
 *
 * Format: "warm <n>" or "cold <n>" (the `boot` field of the status).
 */
const char *diag_boot_str(void);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_DIAG_H_ */
