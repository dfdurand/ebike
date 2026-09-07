/*
 * eBike tracker — AWS IoT Core connection (MQTT/TLS) through the `aws_iot`
 * library.
 *
 * Application topics:
 *   bike/<deviceId>/status    periodic device status (JSON)
 *   bike/<deviceId>/telemetry batched position trace (JSON)
 *   bike/<deviceId>/cmd       commands from the cloud -> CH_CLOUD_CMD
 *   bike/<deviceId>/alert     alerts (CH_ALERT listener)
 *
 * The module builds the aggregated telemetry itself (reading the zbus channels
 * and the GNSS trace) and publishes it at the interval set by the state
 * machine through cloud_set_publish_interval().
 */

#ifndef EBIKE_CLOUD_H_
#define EBIKE_CLOUD_H_

#include <stdbool.h>
#include <stdint.h>

#include "events/channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Provision the AWS credentials (root CA, device certificate, private
 *        key) into the modem under CONFIG_MQTT_HELPER_SEC_TAG.
 *
 * Only writes what is missing or different. The modem must be OFFLINE
 * (CFUN=0/4): call after connectivity_init() but BEFORE
 * connectivity_connect().
 *
 * @retval 0 on success (or when provisioning is disabled), negative errno
 *         otherwise.
 */
int cloud_provision_credentials(void);

/**
 * @brief Initialise the aws_iot library and declare the application topics.
 *
 * Call once, before cloud_connect().
 *
 * @retval 0 on success, negative errno otherwise.
 */
int cloud_init(void);

/**
 * @brief Start connecting to the AWS IoT broker (asynchronous, with retries).
 *
 * Call once LTE is attached. The actual connection is reported through
 * AWS_IOT_EVT_CONNECTED, observable with cloud_is_connected().
 *
 * @retval 0 when the request was accepted, negative errno otherwise.
 */
int cloud_connect(void);

/** @brief Whether the MQTT connection to the broker is established. */
bool cloud_is_connected(void);

/**
 * @brief Set the publication rate of the aggregated telemetry.
 *
 * Called by the state machine when entering RIDING/PARKED/ALARM. Triggers an
 * immediate publication then reschedules the cycle at the new rate. Nothing is
 * published until it has been called once.
 *
 * @param seconds Interval between publications (> 0).
 */
void cloud_set_publish_interval(uint32_t seconds);

/**
 * @brief Report the current state into the Device Shadow (state.reported).
 *
 * Called by the state machine after applying a configuration pushed by the
 * shadow (which settles the delta on the AWS side) and on state transitions.
 *
 * @param cfg Effective configuration (armed, rate, threshold).
 * @param sm_state Name of the current state ("parked", "riding", "alarm").
 * @retval 0 when accepted for sending, -ENOTCONN when offline, errno on error.
 */
int cloud_report_shadow(const struct msg_config *cfg, const char *sm_state);

/**
 * @brief Anchor the home point in the Device Shadow (state.desired).
 *
 * Called when a {"set_home":true} succeeds: the desired state becomes the
 * cloud backup of the home point. NVS is wiped by the mass erase of
 * `west flash --recover`, whereas the shadow survives and pushes
 * home_lat/home_lon back to the device through get/accepted on the next boot.
 *
 * @retval 0 when accepted for sending, -ENOTCONN when offline, errno on error.
 */
int cloud_desire_home(double lat, double lon);

/**
 * @brief Publish a JSON string on bike/<deviceId>/status (QoS 1).
 *
 * @retval 0 when accepted for sending, -ENOTCONN when offline, errno on error.
 */
int cloud_publish_status(const char *json);

/**
 * @brief Publish a JSON string on bike/<deviceId>/telemetry (QoS 1).
 *
 * @retval 0 when accepted for sending, -ENOTCONN when offline, errno on error.
 */
int cloud_publish_telemetry(const char *json);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_CLOUD_H_ */
