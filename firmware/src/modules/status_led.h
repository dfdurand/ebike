/*
 * eBike tracker — status LEDs, the only diagnostic output on battery.
 *
 * WHY THIS MODULE. Away from the desk there is no console: the DK's UART goes
 * through the interface MCU, which needs a USB host to expose it. On a power
 * bank the board is therefore mute, and "the dashboard says offline" covers
 * two completely different failures — the firmware not running at all, and
 * the firmware running fine but the modem never attaching. LED5 is no help:
 * it belongs to the interface MCU and blinks whenever there is no USB host,
 * which is precisely the situation being diagnosed.
 *
 * WHY ONLY TWO LEDS. All four application LEDs sit on Arduino header pins
 * (LED1=D0, LED2=D1, LED3=D4, LED4=D5) and the X-NUCLEO-IKS01A1 shield is
 * mounted on those headers. Measured on the bench, LED3 and LED4 stay dark
 * even during the boot lamp test: D4/D5 are held by the shield and no
 * firmware can light them. The whole diagnosis therefore has to fit on LED1
 * and LED2, which are free.
 *
 * Two independent channels, each read at a glance:
 *
 *   LED1 — the board is alive, and whether it knows where it is
 *     brief blip every second   running, no fresh GNSS fix
 *     even 1 Hz blink           running, fix newer than
 *                               EBIKE_STATUS_LED_FIX_FRESH_S
 *     dark                      nothing runs: power or boot problem
 *
 *   LED2 — how far up the connection ladder it got
 *     dark                      LTE not attached
 *     even 1 Hz blink           LTE attached, broker not reached
 *     solid                     MQTT connection to AWS IoT established
 *
 * LED1 never stops blinking: liveness stays readable whatever the GNSS does.
 *
 * At boot both LEDs are lit together for EBIKE_STATUS_LED_LAMP_TEST_MS. That
 * is the earliest sign of life the board can give on battery, and it tells a
 * dead indicator from a false one — a LED that misses the lamp test is
 * unusable, not a state that never happened.
 *
 * The tick runs on the system workqueue and starts at the very beginning of
 * main(), before the modem init, the credential provisioning and the LTE
 * attach: those steps are long and are exactly where a boot can stall, so the
 * heartbeat must already be blinking while they run.
 */

#ifndef EBIKE_STATUS_LED_H_
#define EBIKE_STATUS_LED_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the status LEDs and start the refresh tick.
 *
 * Call first in main(), before any long boot step. Never fatal: if a LED is
 * unavailable the firmware runs on without the indicator.
 *
 * @retval 0 on success (or when status LEDs are disabled), negative errno
 *         otherwise.
 */
int status_led_init(void);

#ifdef __cplusplus
}
#endif

#endif /* EBIKE_STATUS_LED_H_ */
