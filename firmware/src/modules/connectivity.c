/*
 * eBike tracker — connectivity module implementation.
 */

#include "connectivity.h"
#include "events/channels.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/at_monitor.h>
#include <nrf_modem_at.h>

#include <string.h>

LOG_MODULE_REGISTER(connectivity, CONFIG_EBIKE_LOG_LEVEL);

/* Index -> physical unit conversions (see the modem AT command reference).
 * Duplicated here so this module does not pull in modem_info. */
#define RSRP_IDX_TO_DBM(rsrp) ((rsrp) < 0 ? (rsrp) - 140 : (rsrp) - 141)
#define RSRQ_IDX_TO_DB(rsrq)                                                   \
	((rsrq) < 0 ? (((float)(rsrq) - 39) * 0.5f)                            \
		    : ((rsrq) < 35 ? (((float)(rsrq) - 40) * 0.5f)              \
				   : (((float)(rsrq) - 41) * 0.5f)))

/* Given as soon as the modem registers (home or roaming). */
static K_SEM_DEFINE(lte_connected_sem, 0, 1);

/* lte_lc subscribes with AT+CEREG=5, so a rejected registration carries
 * <cause_type>,<reject_cause> (the EMM cause) in the URC. Log the raw line to
 * capture that cause even when it is not surfaced as an event. */
static void at_cereg_mon_handler(const char *notif);
AT_MONITOR(ebike_cereg_mon, "+CEREG", at_cereg_mon_handler);

static void trim_crlf(char *s)
{
	size_t len = strlen(s);

	while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
		s[--len] = '\0';
	}
}

static void at_cereg_mon_handler(const char *notif)
{
	char line[128];

	strncpy(line, notif, sizeof(line) - 1);
	line[sizeof(line) - 1] = '\0';
	trim_crlf(line);

	LOG_INF("URC: %s", line);
}

/* Run an AT command and log the raw answer (diagnostics only). */
static void at_cmd_log(const char *label, const char *cmd)
{
	char resp[256];
	int err;

	err = nrf_modem_at_cmd(resp, sizeof(resp), "%s", cmd);
	if (err) {
		LOG_WRN("%s: failed (%d)", label, err);
		return;
	}

	trim_crlf(resp);
	/* Keep the answer on a single log line: inner CR/LF become spaces, so
	 * no bare "OK" line reaches the console where flash.sh could mistake it
	 * for a shell acknowledgement. */
	for (char *p = resp; *p != '\0'; p++) {
		if (*p == '\r' || *p == '\n') {
			*p = ' ';
		}
	}
	LOG_INF("%s: %s", label, resp);
}

static volatile bool is_connected;

/* Last measured RSRP (dBm), 0 when no valid measurement yet. */
static volatile int last_rsrp_dbm;

static const char *nw_reg_status_str(enum lte_lc_nw_reg_status status)
{
	switch (status) {
	case LTE_LC_NW_REG_NOT_REGISTERED:
		return "not registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:
		return "registered - home";
	case LTE_LC_NW_REG_SEARCHING:
		return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:
		return "registration denied";
	case LTE_LC_NW_REG_UNKNOWN:
		return "unknown (out of coverage?)";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		return "registered - roaming";
	case LTE_LC_NW_REG_UICC_FAIL:
		return "UICC fail (SIM?)";
	default:
		return "other";
	}
}

static const char *lte_mode_str(enum lte_lc_lte_mode mode)
{
	switch (mode) {
	case LTE_LC_LTE_MODE_LTEM:
		return "LTE-M";
	case LTE_LC_LTE_MODE_NBIOT:
		return "NB-IoT";
	case LTE_LC_LTE_MODE_NONE:
		return "none";
	default:
		return "?";
	}
}

/* lte_lc event handler — runs in the lte_lc thread. */
static void lte_evt_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("Network: %s", nw_reg_status_str(evt->nw_reg_status));

		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			is_connected = true;
			k_sem_give(&lte_connected_sem);
		} else {
			is_connected = false;
		}

		/* Notify the state machine. K_NO_WAIT: the lte_lc handler
		 * context must never block. */
		{
			const struct msg_lte_status msg = {
				.connected = is_connected,
			};

			(void)zbus_chan_pub(&CH_LTE_STATUS, &msg, K_NO_WAIT);
		}
		break;

	case LTE_LC_EVT_LTE_MODE_UPDATE:
		LOG_INF("Active LTE mode: %s", lte_mode_str(evt->lte_mode));
		break;

	case LTE_LC_EVT_RRC_UPDATE:
		LOG_DBG("RRC: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected"
								   : "idle");
		break;

	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("Cell: ID 0x%08X, TAC 0x%04X", evt->cell.id,
			evt->cell.tac);
		break;

#if defined(CONFIG_LTE_LC_MODEM_EVENTS_MODULE)
	case LTE_LC_EVT_MODEM_EVENT:
		switch (evt->modem_evt.type) {
		case LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE:
			LOG_INF("Modem: light search done (nothing found, "
				"deep search running)");
			break;
		case LTE_LC_MODEM_EVT_SEARCH_DONE:
			LOG_INF("Modem: network search done");
			break;
		case LTE_LC_MODEM_EVT_RESET_LOOP:
			LOG_WRN("Modem: RESET LOOP detected — attach blocked "
				"for 30 min");
			break;
		case LTE_LC_MODEM_EVT_BATTERY_LOW:
			LOG_WRN("Modem: low battery voltage — modem disabled");
			break;
		case LTE_LC_MODEM_EVT_OVERHEATED:
			LOG_WRN("Modem: overheated — modem disabled");
			break;
		case LTE_LC_MODEM_EVT_NO_IMEI:
			LOG_ERR("Modem: no IMEI");
			break;
		default:
			LOG_INF("Modem: event %d", evt->modem_evt.type);
			break;
		}
		break;
#endif /* CONFIG_LTE_LC_MODEM_EVENTS_MODULE */

	default:
		break;
	}
}

int connectivity_init(void)
{
	int err;

	LOG_INF("Initialising modem (nrf_modem_lib)...");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init() failed: %d", err);
		return err;
	}

#if defined(CONFIG_LTE_LC_PDN_MODULE)
	/* Configure the default PDP context (CID 0) before attaching. With an
	 * empty CONFIG_EBIKE_APN the network supplies the APN itself, which is
	 * what most LTE-M IoT SIMs expect.
	 *
	 * PDP family: IPv4v6 by default, IPv4 only as a fallback — some visited
	 * networks reject the IPV4V6 type. */
	enum lte_lc_pdn_family fam = IS_ENABLED(CONFIG_EBIKE_PDN_IPV4_ONLY)
					     ? LTE_LC_PDN_FAM_IPV4
					     : LTE_LC_PDN_FAM_IPV4V6;

	if (CONFIG_EBIKE_APN[0] != '\0') {
		err = lte_lc_pdn_ctx_configure(0, CONFIG_EBIKE_APN, fam, NULL);
		if (err) {
			LOG_WRN("APN '%s' configuration failed: %d "
				"(falling back to auto)", CONFIG_EBIKE_APN, err);
		} else {
			LOG_INF("APN forced: '%s' (family %s)", CONFIG_EBIKE_APN,
				fam == LTE_LC_PDN_FAM_IPV4 ? "IPv4" : "IPv4v6");
		}
	} else {
		LOG_INF("APN: auto (supplied by the network)");
	}
#else
	LOG_INF("APN: auto (supplied by the network)");
#endif

	return 0;
}

int connectivity_connect(void)
{
	int err;

	LOG_INF("Connecting to LTE-M (asynchronous)...");
	err = lte_lc_connect_async(lte_evt_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async() failed: %d", err);
		return err;
	}

	return 0;
}

int connectivity_wait_for_connection(int timeout_ms)
{
	k_timeout_t timeout =
		(timeout_ms > 0) ? K_MSEC(timeout_ms) : K_FOREVER;

	if (k_sem_take(&lte_connected_sem, timeout) != 0) {
		return -ETIMEDOUT;
	}

	/* Re-arm for future connection losses and recoveries. */
	k_sem_give(&lte_connected_sem);
	k_sem_take(&lte_connected_sem, K_NO_WAIT);

	return 0;
}

bool connectivity_is_connected(void)
{
	return is_connected;
}

int connectivity_last_rsrp_dbm(void)
{
	return last_rsrp_dbm;
}

void connectivity_log_status(void)
{
	enum lte_lc_nw_reg_status status;
	int err;

	err = lte_lc_nw_reg_status_get(&status);
	if (err) {
		LOG_WRN("Reading network status failed: %d", err);
		return;
	}

	LOG_INF("Network status: %s", nw_reg_status_str(status));

	/* While unregistered, dump the last EMM/ESM failure cause (+CEER) and
	 * the full network view (%XMONITOR) to make attach problems diagnosable
	 * from the console alone. */
	if (status != LTE_LC_NW_REG_REGISTERED_HOME &&
	    status != LTE_LC_NW_REG_REGISTERED_ROAMING) {
		at_cmd_log("CEER", "AT+CEER");
		at_cmd_log("XMONITOR", "AT%XMONITOR");
	}
}

/* Documented reasons for a positive lte_lc_conn_eval_params_get() return. */
static const char *conn_eval_reason_str(int ret)
{
	switch (ret) {
	case 1:
		return "no cell available";
	case 2:
		return "UICC not available";
	case 3:
		return "only barred cells available";
	case 4:
		return "radio busy (GNSS activity)";
	case 5:
		return "aborted, higher priority operation";
	case 6:
		return "not registered to the network";
	case 7:
		return "unspecified";
	default:
		return "unknown reason";
	}
}

int connectivity_log_signal_quality(void)
{
	struct lte_lc_conn_eval_params params = {0};
	int ret;

	ret = lte_lc_conn_eval_params_get(&params);
	if (ret > 0) {
		/* The modem declined to evaluate, for a documented and usually
		 * benign reason. Reason 4 ("radio busy") is the normal state
		 * while GNSS priority mode hunts a first fix, so it fires on
		 * every publish cycle and says nothing about the link: debug
		 * level, not a warning. The RSRP simply keeps its last
		 * measured value until the radio frees up. */
		LOG_DBG("Connection evaluation unavailable: %s (%d)",
			conn_eval_reason_str(ret), ret);
		return ret;
	}
	if (ret < 0) {
		/* Negative is a real call failure, not a modem verdict. */
		LOG_WRN("Connection evaluation call failed (%d)", ret);
		return ret;
	}

	if (params.rsrp == LTE_LC_CELL_RSRP_INVALID) {
		LOG_INF("Signal: RSRP unavailable");
	} else {
		last_rsrp_dbm = RSRP_IDX_TO_DBM(params.rsrp);
		LOG_INF("Signal: RSRP %d dBm (idx %d), RSRQ %.1f dB (idx %d)",
			last_rsrp_dbm, params.rsrp,
			(double)RSRQ_IDX_TO_DB(params.rsrq), params.rsrq);
	}

	LOG_INF("Cell: EARFCN %d, pathloss %d dB", params.earfcn,
		params.dl_pathloss);

	return 0;
}
