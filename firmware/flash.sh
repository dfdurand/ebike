#!/usr/bin/env bash
# Graceful flash of the eBike firmware (nRF9161-DK).
#
# Powers the modem down (AT+CFUN=0 through the device shell on the NS console)
# BEFORE the flash reset, for two reasons:
#   1. the modem keeps a persistent RESET LOOP counter; a few abrupt resets in
#      a row freeze the LTE attach for 30 minutes;
#   2. a clean power-off saves the network cache to NVM, so the next boot
#      re-attaches in seconds instead of minutes.
#
# Usage: ./flash.sh                         # flashes ./build
#        PORT=/dev/ttyACM0 ./flash.sh       # pick the console port
#        SN=<serial> ./flash.sh              # pick a board by serial number
#
# If the running firmware has no shell, or the board does not answer, the
# script says so and flashes anyway after the timeout.

set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-/dev/ttyACM0}"
SN="${SN:-}"

# west lives in the NCS bundle, so env.sh must be sourced if it is not already.
if ! command -v west >/dev/null 2>&1; then
	# shellcheck source=/dev/null
	source ../env.sh
fi

graceful_modem_off() {
	[ -c "$PORT" ] || { echo "$PORT missing — board unplugged?"; return 1; }

	stty -F "$PORT" 115200 raw -echo

	# Open the port for reading BEFORE sending anything, otherwise the reply
	# can be missed.
	#
	# `lte power_off` goes through lte_lc, which re-arms its URC
	# subscriptions if the flash is aborted. It stays silent on success, so
	# `at AT+CFUN=0` follows as an acknowledgement: it is idempotent and
	# answers OK even when the modem is already off.
	#
	# Never cycle CFUN in raw AT without rebooting afterwards: it drops the
	# URC subscriptions lte_lc registered, and the application goes blind to
	# network events.
	exec 3<"$PORT"
	printf '\nlte power_off\n' > "$PORT"
	sleep 1
	printf 'at AT+CFUN=0\n' > "$PORT"
	if timeout 10 grep -qm1 "^OK" <&3; then
		exec 3<&-
		echo "Modem powered down (CFUN=0) — graceful reset."
		return 0
	fi
	exec 3<&-
	return 1
}

if ! graceful_modem_off; then
	echo "No OK received (firmware without shell? console busy?)."
	echo "Flashing anyway — this reset counts towards the RESET LOOP limit."
fi

if [ -n "$SN" ]; then
	west flash --recover --dev-id "$SN"
else
	west flash --recover
fi
