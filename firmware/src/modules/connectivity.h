/*
 * eBike tracker — connectivity module: LTE-M network attach.
 *
 * Wraps lte_lc: optional APN configuration, LTE-M mode, asynchronous connect,
 * registration tracking and reconnection. Exposes the minimal API that main.c
 * and the cloud module need.
 */

#ifndef EBIKE_CONNECTIVITY_H_
#define EBIKE_CONNECTIVITY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the modem, offline.
 *
 * Initialises nrf_modem_lib and applies CONFIG_EBIKE_APN when set. The modem
 * stays in CFUN=0, which is the window where TLS credentials can be written
 * (cloud_provision_credentials()).
 *
 * @retval 0 on success, negative errno otherwise.
 */
int connectivity_init(void);

/**
 * @brief Start the LTE-M connection (asynchronous).
 *
 * Registers the lte_lc event handler and calls lte_lc_connect_async(). Returns
 * immediately; the actual attach is reported through events.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int connectivity_connect(void);

/**
 * @brief Block until the modem is registered (home or roaming).
 *
 * @param timeout_ms Maximum wait in milliseconds (0 waits forever).
 * @retval 0 when registered, -ETIMEDOUT on timeout.
 */
int connectivity_wait_for_connection(int timeout_ms);

/** @brief Whether the modem is currently registered. */
bool connectivity_is_connected(void);

/**
 * @brief Log the current registration status (AT+CEREG).
 *
 * Useful to follow a long "searching" phase without relying only on state
 * change events.
 */
void connectivity_log_status(void);

/**
 * @brief Query and log the current signal quality (RSRP/RSRQ).
 *
 * Uses lte_lc_conn_eval_params_get(). Only meaningful once connected.
 *
 * @retval 0 when the evaluation succeeded, non-zero otherwise.
 */
int connectivity_log_signal_quality(void);

/**
 * @brief Last measured RSRP in dBm (0 when nothing has been measured yet).
 *
 * Updated by every successful connectivity_log_signal_quality().
 */
int connectivity_last_rsrp_dbm(void);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_CONNECTIVITY_H_ */
