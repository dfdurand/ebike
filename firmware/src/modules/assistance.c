/*
 * eBike tracker — A-GNSS assistance implementation (nRF Cloud CoAP).
 *
 * Derived from nrf/samples/cellular/gnss (assistance.c), reduced to A-GNSS
 * (no P-GPS) and wrapped: the blocking fetch runs on a dedicated workqueue and
 * the request is copied and posted asynchronously.
 */

#include "assistance.h"

#if defined(CONFIG_EBIKE_AGNSS)

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/lte_lc.h>
#include <modem/modem_info.h>

#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_agnss.h>
#include <date_time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(assistance, CONFIG_EBIKE_LOG_LEVEL);

/* Retries: the nRF Cloud JWT needs a valid wall clock, which is not
 * synchronised at boot (modem NITZ or NTP). Rather than fail, wait for the
 * time and try again; transient network failures are retried the same way.
 * 12 x 20 s is about four minutes of margin before giving up, after which
 * GNSS continues on its own. */
#define ASSIST_RETRY_DELAY  K_SECONDS(20)
#define ASSIST_MAX_ATTEMPTS 12

/* Reception buffer for the A-GNSS data, sized by the library. */
static char agnss_buf[NRF_CLOUD_AGNSS_MAX_DATA_SIZE];

/* Request being processed (copied from the GNSS context). */
static struct nrf_modem_gnss_agnss_data_frame agnss_req;

static volatile bool assistance_active;
static int assist_attempts;

/* Instrumentation: enough to explain a long TTFF remotely, without a console.
 * See assistance_status_str() in assistance.h. */
static int64_t assist_started_ms = -1;
static char assist_status[32] = "-";

/* Dedicated workqueue: the fetch (CoAP/DTLS plus transfer) blocks and must not
 * freeze the system workqueue, which handles GNSS PVT frames and telemetry
 * publication. Delayable so it can wait for time sync and retry. */
static K_THREAD_STACK_DEFINE(assist_stack, CONFIG_EBIKE_AGNSS_STACK_SIZE);
static struct k_work_q assist_wq;
static struct k_work_delayable assist_work;

/* Serving cell -> approximate position on the nRF Cloud side, which speeds up
 * the TTFF. Best effort: on failure A-GNSS still works, just without the
 * position hint. */
static int serving_cell_info_get(struct lte_lc_cell *cell)
{
	char resp[MODEM_INFO_MAX_RESPONSE_SIZE];
	int err;

	err = modem_info_string_get(MODEM_INFO_CELLID, resp, sizeof(resp));
	if (err < 0) {
		return err;
	}
	cell->id = strtol(resp, NULL, 16);

	err = modem_info_string_get(MODEM_INFO_AREA_CODE, resp, sizeof(resp));
	if (err < 0) {
		return err;
	}
	cell->tac = strtol(resp, NULL, 16);

	/* MODEM_INFO_OPERATOR returns MCC and MNC concatenated (5-6 digits). */
	err = modem_info_string_get(MODEM_INFO_OPERATOR, resp, sizeof(resp));
	if (err < 0) {
		return err;
	}
	cell->mnc = strtol(&resp[3], NULL, 10);
	resp[3] = '\0';
	cell->mcc = strtol(resp, NULL, 10);

	return 0;
}

/* Schedule another attempt, or give up after ASSIST_MAX_ATTEMPTS. */
static void assist_retry_or_giveup(const char *reason)
{
	if (++assist_attempts < ASSIST_MAX_ATTEMPTS) {
		LOG_DBG("A-GNSS: %s — retrying (%d/%d)", reason,
			assist_attempts, ASSIST_MAX_ATTEMPTS);
		snprintf(assist_status, sizeof(assist_status), "wait a%d",
			 assist_attempts);
		(void)k_work_reschedule_for_queue(&assist_wq, &assist_work,
						  ASSIST_RETRY_DELAY);
	} else {
		LOG_WRN("A-GNSS: %s — giving up after %d attempts, autonomous "
			"GNSS", reason, ASSIST_MAX_ATTEMPTS);
		snprintf(assist_status, sizeof(assist_status), "fail a%d",
			 assist_attempts);
		assistance_active = false;
	}
}

static void assist_work_fn(struct k_work *work)
{
	struct lte_lc_cells_info net_info = {0};
	struct nrf_cloud_coap_agnss_request request = {
		.type = NRF_CLOUD_COAP_AGNSS_REQ_CUSTOM,
		.agnss_req = &agnss_req,
		.net_info = NULL,
		.filtered = false,
		.mask_angle = NRF_CLOUD_AGNSS_MASK_ANGLE_NONE,
	};
	struct nrf_cloud_coap_agnss_result result = {
		.buf = agnss_buf,
		.buf_sz = sizeof(agnss_buf),
		.agnss_sz = 0,
	};
	int err;

	/* The nRF Cloud auth JWT needs a valid wall clock, which is not yet
	 * synchronised at boot (failures show up as -22 or -13). Wait rather
	 * than burn attempts on an invalid token. */
	if (!date_time_is_valid()) {
		assist_retry_or_giveup("waiting for time sync");
		return;
	}

	err = nrf_cloud_coap_connect(NULL);
	if (err) {
		assist_retry_or_giveup("nRF Cloud connection");
		return;
	}

	if (serving_cell_info_get(&net_info.current_cell) == 0) {
		request.net_info = &net_info;
	}

	LOG_INF("A-GNSS: requesting data from nRF Cloud (data_flags 0x%02x)",
		agnss_req.data_flags);

	err = nrf_cloud_coap_agnss_data_get(&request, &result);
	if (err == 0) {
		err = nrf_cloud_agnss_process(result.buf, result.agnss_sz);
	}

	/* Close the connection after every fetch: it frees modem resources, and
	 * a second permanent DTLS session would compete with the AWS link.
	 * A-GNSS requests are rare (cold start, then ephemeris expiry). */
	(void)nrf_cloud_coap_disconnect();

	if (err) {
		assist_retry_or_giveup("download or injection failed");
		return;
	}

	int64_t elapsed_s = (k_uptime_get() - assist_started_ms) / MSEC_PER_SEC;

	LOG_INF("A-GNSS injected (%zu bytes in %lld s) — reduced TTFF",
		result.agnss_sz, elapsed_s);
	snprintf(assist_status, sizeof(assist_status), "ok %zub %llds",
		 result.agnss_sz, elapsed_s);
	assistance_active = false;
}

int assistance_init(void)
{
	int err;

	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init: %d", err);
		return err;
	}

	err = modem_info_init();
	if (err) {
		LOG_WRN("modem_info_init: %d (no position assistance)", err);
	}

	k_work_queue_start(&assist_wq, assist_stack,
			   K_THREAD_STACK_SIZEOF(assist_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&assist_wq.thread, "ebike_agnss");
	k_work_init_delayable(&assist_work, assist_work_fn);

	return 0;
}

int assistance_request(const struct nrf_modem_gnss_agnss_data_frame *req)
{
	if (assistance_active) {
		return -EBUSY;
	}

	assistance_active = true;
	assist_attempts = 0;
	assist_started_ms = k_uptime_get();
	strcpy(assist_status, "wait a0");
	memcpy(&agnss_req, req, sizeof(agnss_req));

	if (k_work_reschedule_for_queue(&assist_wq, &assist_work, K_NO_WAIT) < 0) {
		assistance_active = false;
		strcpy(assist_status, "fail a0");
		return -EBUSY;
	}

	return 0;
}

bool assistance_is_active(void)
{
	return assistance_active;
}

const char *assistance_status_str(void)
{
	return assist_status;
}

#else /* CONFIG_EBIKE_AGNSS */

const char *assistance_status_str(void)
{
	return "off";
}

#endif /* CONFIG_EBIKE_AGNSS */
