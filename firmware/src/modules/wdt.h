/*
 * eBike tracker — hardware watchdog through the Zephyr task watchdog.
 *
 * WHAT THIS COVERS. Fatal errors are already reported by diag.c. The blind
 * spot was the SILENT hang: a thread stuck on a socket that never returns, a
 * mutex never released, a loop that stops progressing. No fault, no reboot —
 * the bike simply goes quiet and nothing explains why.
 *
 * The task watchdog fixes that with software channels layered on the single
 * nRF91 hardware watchdog: every loop that MUST run continuously feeds its
 * channel, and the SoC restarts as soon as one of them goes silent.
 *
 * Channels come in two families with deliberately different timeouts, and that
 * split is the whole tuning:
 *
 *   - Short loops (state machine, system workqueue): EBIKE_WDT_TIMEOUT_S,
 *     tight. These contexts only run bounded work, so exceeding 30 s means
 *     something really is stuck.
 *
 *   - Cloud workqueue: EBIKE_WDT_CLOUD_TIMEOUT_S, deliberately permissive.
 *     aws_iot_connect() (DNS plus TLS handshake) can legitimately block for
 *     tens of seconds on a degraded network. A tight timeout there would turn
 *     every difficult reconnection into a reboot — exactly the crash loop this
 *     is meant to prevent.
 *
 * That separation is why the cloud path moved off the system workqueue: while
 * they shared one context, the choice was between a useless watchdog (timeout
 * sized for TLS) and spurious reboots.
 */

#ifndef EBIKE_WDT_H_
#define EBIKE_WDT_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the hardware watchdog and the channels of the internal loops.
 *
 * Installs the state machine channel (fed by wdt_feed_main()) and the system
 * workqueue channel (fed by an internal periodic beat).
 *
 * The nRF91 watchdog can neither be stopped nor reconfigured once started, so
 * call this only after a nominal boot and never before the watched loops are
 * running. It pauses while a debugger halts the core
 * (WDT_OPT_PAUSE_HALTED_BY_DBG), so single-stepping still works.
 *
 * @retval 0 on success (or when the watchdog is disabled), negative errno
 *         otherwise.
 */
int wdt_init(void);

/**
 * @brief Watch an application workqueue with its own timeout.
 *
 * Registers a dedicated channel and posts a periodic beat to the queue: the
 * queue is considered alive as long as it executes that work. The timeout is
 * per queue, which allows tolerating a legitimately slow path (TLS) without
 * relaxing the supervision of the short loops.
 *
 * Call after k_work_queue_start() on the queue concerned.
 *
 * @param q Workqueue to watch (must stay valid, typically static).
 * @param timeout_s Time without a beat before reset (seconds).
 * @param name Short name identifying the queue in the reset log.
 * @retval 0 on success (or when the watchdog is disabled), negative errno
 *         otherwise.
 */
int wdt_watch_queue(struct k_work_q *q, uint32_t timeout_s, const char *name);

/**
 * @brief Feed the state machine event loop channel.
 *
 * Call on every turn of the main loop, which wakes up at least every 5 s
 * (the zbus_sub_wait timeout), well below the channel timeout.
 */
void wdt_feed_main(void);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_WDT_H_ */
