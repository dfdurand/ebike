/*
 * eBike tracker — watchdog module implementation.
 */

#include "wdt.h"

#if defined(CONFIG_EBIKE_WDT)

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

LOG_MODULE_REGISTER(wdt, CONFIG_EBIKE_LOG_LEVEL);

#define FEED_PERIOD K_SECONDS(CONFIG_EBIKE_WDT_FEED_S)

/* State machine loop channel, fed from main.c. */
static int main_channel = -1;

/* System workqueue beat: the mere fact that this work runs proves the queue is
 * not blocked. */
static void sys_beat_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sys_beat, sys_beat_fn);
static int sys_channel = -1;

/* Watched application workqueues (today: the cloud queue).
 *
 * Registration is DEFERRED until wdt_init() has run, and that is not a style
 * choice: cloud_init() asks to be watched long before the boot completes,
 * whereas the watchdog must only start at the very end (the LTE attach and
 * certificate provisioning are slow and feed nothing). task_wdt_add() does not
 * check that the subsystem is initialised: called too early it registers a
 * channel whose deadline is K_TICKS_FOREVER and which cannot be fed, and the
 * first schedule_next_timeout() of task_wdt_init() sees it as already expired
 * — an immediate reboot, in a loop. So the request is recorded and honoured
 * once the watchdog actually starts. */
#define WATCHED_QUEUES_MAX 2

struct watched_queue {
	struct k_work_delayable beat;
	struct k_work_q *q;
	const char *name;
	uint32_t timeout_s;
	int channel;
};

static struct watched_queue watched[WATCHED_QUEUES_MAX];
static int watched_count;

/* task_wdt initialised and channels feedable. */
static bool started;

/* ISR context (k_timer callback): only trace, then restart. LOG_PANIC() forces
 * a synchronous flush of the deferred logs — without it the message naming the
 * culprit would go down with the reset. */
static void starved_cb(int channel_id, void *user_data)
{
	const char *name = (const char *)user_data;

	LOG_ERR("Watchdog: '%s' stopped responding (channel %d) — restarting",
		name != NULL ? name : "?", channel_id);
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
}

static void sys_beat_fn(struct k_work *work)
{
	(void)task_wdt_feed(sys_channel);
	(void)k_work_schedule(&sys_beat, FEED_PERIOD);
}

static void queue_beat_fn(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct watched_queue *wq = CONTAINER_OF(dwork, struct watched_queue, beat);

	(void)task_wdt_feed(wq->channel);
	(void)k_work_reschedule_for_queue(wq->q, &wq->beat, FEED_PERIOD);
}

/* Open the channel of a recorded queue and start its beat. */
static int queue_arm(struct watched_queue *wq)
{
	wq->channel = task_wdt_add(wq->timeout_s * MSEC_PER_SEC, starved_cb,
				   (void *)wq->name);
	if (wq->channel < 0) {
		LOG_ERR("Watchdog channel '%s': %d", wq->name, wq->channel);
		return wq->channel;
	}

	k_work_init_delayable(&wq->beat, queue_beat_fn);
	(void)k_work_reschedule_for_queue(wq->q, &wq->beat, FEED_PERIOD);

	LOG_INF("Watchdog: '%s' watched (%u s)", wq->name, wq->timeout_s);

	return 0;
}

int wdt_init(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	int err;

	if (!device_is_ready(hw_wdt)) {
		LOG_ERR("Hardware watchdog unavailable — supervision inactive");
		return -ENODEV;
	}

	err = task_wdt_init(hw_wdt);
	if (err) {
		LOG_ERR("task_wdt_init: %d", err);
		return err;
	}

	started = true;

	main_channel = task_wdt_add(CONFIG_EBIKE_WDT_TIMEOUT_S * MSEC_PER_SEC,
				    starved_cb, "state machine");
	if (main_channel < 0) {
		LOG_ERR("Watchdog channel 'state machine': %d", main_channel);
		return main_channel;
	}

	sys_channel = task_wdt_add(CONFIG_EBIKE_WDT_TIMEOUT_S * MSEC_PER_SEC,
				   starved_cb, "system workqueue");
	if (sys_channel < 0) {
		LOG_ERR("Watchdog channel 'system workqueue': %d", sys_channel);
		return sys_channel;
	}

	(void)k_work_schedule(&sys_beat, FEED_PERIOD);

	/* Queues registered during boot, before the watchdog was running. */
	for (int i = 0; i < watched_count; i++) {
		(void)queue_arm(&watched[i]);
	}

	LOG_INF("Watchdog active — short loops %d s, beat %d s",
		CONFIG_EBIKE_WDT_TIMEOUT_S, CONFIG_EBIKE_WDT_FEED_S);

	return 0;
}

int wdt_watch_queue(struct k_work_q *q, uint32_t timeout_s, const char *name)
{
	struct watched_queue *wq;

	if (q == NULL || watched_count >= WATCHED_QUEUES_MAX) {
		return -ENOMEM;
	}

	wq = &watched[watched_count++];
	wq->q = q;
	wq->name = name;
	wq->timeout_s = timeout_s;
	wq->channel = -1;

	/* Before wdt_init(): the request is recorded and honoured when the
	 * watchdog starts (see the comment on watched[]). */
	if (!started) {
		LOG_DBG("Watchdog: '%s' registered, waiting for start", name);
		return 0;
	}

	return queue_arm(wq);
}

void wdt_feed_main(void)
{
	if (main_channel >= 0) {
		(void)task_wdt_feed(main_channel);
	}
}

#else /* CONFIG_EBIKE_WDT */

int wdt_init(void)
{
	return 0;
}

int wdt_watch_queue(struct k_work_q *q, uint32_t timeout_s, const char *name)
{
	ARG_UNUSED(q);
	ARG_UNUSED(timeout_s);
	ARG_UNUSED(name);
	return 0;
}

void wdt_feed_main(void)
{
}

#endif /* CONFIG_EBIKE_WDT */
