/*
 * eBike tracker — A-GNSS assistance from nRF Cloud over CoAP.
 *
 * The GNSS modem announces which assistance data it needs through
 * NRF_MODEM_GNSS_EVT_AGNSS_REQ (ephemerides, almanac, time, approximate
 * position). This module downloads it from nRF Cloud over CoAP as a one-shot
 * fetch and injects it into the modem, which brings the cold time-to-first-fix
 * down from minutes to seconds.
 *
 * Independent of the AWS link: nRF Cloud is only contacted for assistance, and
 * the connection is closed straight after. Requires the board to be onboarded
 * on nRF Cloud with credentials in CONFIG_NRF_CLOUD_SEC_TAG. Without them the
 * request fails cleanly and GNSS falls back to an autonomous search.
 */

#ifndef EBIKE_ASSISTANCE_H_
#define EBIKE_ASSISTANCE_H_

#include <stdbool.h>

#include <nrf_modem_gnss.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the nRF Cloud CoAP client and the dedicated workqueue.
 *
 * Call once, with the modem initialised (typically from location_start()).
 * Opens no network connection: that happens lazily on the first request.
 *
 * @retval 0 on success, negative errno otherwise.
 */
int assistance_init(void);

/**
 * @brief Handle an assistance request from the GNSS (asynchronous).
 *
 * Copies the request frame and defers the download and injection to the
 * dedicated workqueue, returning immediately: the caller is the GNSS context
 * and must never block on the network.
 *
 * @param req Assistance frame requested by the modem.
 * @retval 0 when accepted, -EBUSY when a request is already in flight.
 */
int assistance_request(const struct nrf_modem_gnss_agnss_data_frame *req);

/** @brief Whether an assistance download is in progress. */
bool assistance_is_active(void);

/**
 * @brief Short textual assistance result, for the MQTT status.
 *
 * A long TTFF is ambiguous without it: nothing in the published data would
 * say whether the injection failed, was late, or was never requested by the
 * modem at all — the diagnosis would only exist in console logs, which is
 * nowhere useful in the field.
 *
 * Values of the `agnss` field:
 *   "-"              no assistance requested by the modem since boot
 *   "wait a<n>"      download in progress, attempt n (waiting for time or
 *                    network)
 *   "ok <b>b <s>s"   injected: bytes received and delay since the request
 *   "fail a<n>"      gave up after n attempts — autonomous GNSS search
 *   "off"            CONFIG_EBIKE_AGNSS disabled
 *
 * Cross-read with location_ttff_s(): a long TTFF with "ok" is a reception
 * problem, the same TTFF with "fail" points at the assistance.
 */
const char *assistance_status_str(void);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_ASSISTANCE_H_ */
