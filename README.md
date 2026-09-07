# eBike Tracker

A connected e-bike tracker built on a Nordic **nRF9161** (LTE-M + GNSS) running
Zephyr / nRF Connect SDK, with a serverless AWS backend and a Python console.

The bike reports where it is, how fast it is going and how it is doing. When it
is locked, any movement raises an alert within seconds. When it leaves the area
you defined, you get an e-mail. All of it keeps working with the network down —
alerts and trace are buffered on the device and replayed on reconnection, with
their real age attached.

```
   nRF9161-DK                 AWS                          you
+---------------+      +----------------+          +------------------+
| GNSS  IMU     |      | IoT Core rules |          | Dash console     |
| reed  LTE-M   |─MQTT─┤  -> DynamoDB   ├──HTTPS───┤ live map, trips, |
| zbus + SMF    | TLS  |  -> Lambda/SNS |          | stats, remote    |
+---------------+      +----------------+          +------------------+
        ^                       |                          |
        |                       v                          |
   Device Shadow          alert e-mail                      |
        ^-----------------------------------------------lock/unlock,
                                                        geofence
```

## Features

| | |
|---|---|
| **Live tracking** | GNSS trace sampled every 5 s while riding, batched into one MQTT message per cycle |
| **Anti-theft** | Locked remotely through the Device Shadow; any wheel or IMU movement raises an immediate alert and close tracking |
| **Geofence** | Computed on device, with spatial and temporal debounce so a bike parked on the boundary does not spam alerts |
| **Offline resilience** | 240-point trace ring buffer and an alert queue, replayed on reconnection with their age |
| **Fast first fix** | A-GNSS assistance fetched from nRF Cloud over CoAP, cutting cold TTFF from minutes to seconds |
| **Power aware** | Reduced publish and sampling rates when parked, PSM requested, exponential backoff with jitter |
| **Self-diagnosing** | Reset cause, fatal crash capture across reboots, warm/cold boot marker, watchdog per critical loop |
| **Readable on battery** | Two status LEDs report liveness, GNSS and the connection ladder when no console is attached |
| **Console** | Live map, per-day trips, speed/IMU/signal charts, and the lock/geofence remote |

## Hardware

| Item | Notes |
|---|---|
| **Nordic nRF9161-DK** (PCA10153) | The board this project targets. An nRF9160-DK works with an overlay change. |
| **X-NUCLEO-IKS01A1 shield** | Only its LSM6DS0 accelerometer is used, on `arduino_i2c` at address 0x6b (SA0 tied to VDD by the shield default). |
| **Reed switch or Hall sensor** | One pulse per wheel turn. |
| **One magnet** | Fixed to a spoke, passing in front of the sensor. |
| **LTE-M SIM** | Any IoT SIM with LTE-M coverage where you ride. A `simbase_cli.py` helper is included for one provider, but nothing depends on it. |
| **LTE and GNSS antennas** | Supplied with the DK. |
| **USB cable or battery** | USB for the bench, a battery pack for real rides. |

### Wiring

The speed sensor goes on **D2 = P0.02**, wired between D2 and GND on the
shield's through-holes. The internal pull-up is enabled and the pulse is an edge
to ground.

D2 is not an arbitrary choice: it is the only Arduino header pin left free on
the DK (D0/D1/D4/D5 are LEDs, D8/D9/A4/A5 are buttons, D11-D13 are SPI3, D14/D15
are the shield I2C, A0-A3 are UART flow control including the console) **and** it
is never used as an interrupt by the IKS01A* shields.

### Status LEDs

Away from the desk there is no console: the DK's UART goes through the
interface MCU, which needs a USB host. Two LEDs carry the diagnosis instead.

| LED | Pattern | Meaning |
|---|---|---|
| **LED1** | dark | nothing runs — power or boot problem |
| | brief blip every second | firmware running, no fresh GNSS fix |
| | even 1 Hz blink | firmware running, fix newer than 120 s |
| **LED2** | dark | LTE not attached |
| | even 1 Hz blink | LTE attached, broker not reached |
| | solid | MQTT connection to AWS IoT established |

Both light together for one second at boot (lamp test). That is the earliest
sign of life the board can give, and it separates a dead indicator from a
state that never happened.

Only LED1 and LED2 are driven: the four application LEDs sit on Arduino header
pins (LED1=D0, LED2=D1, LED3=D4, LED4=D5) and the IKS01A1 shield holds D4/D5
down, so LED3 and LED4 cannot light with the shield fitted. On a board without
it, `CONFIG_EBIKE_STATUS_LED_EXTENDED=y` brings them back.

Note that LED5 belongs to the interface MCU and blinks whenever there is no USB
host — that is, exactly in the situation being diagnosed. Ignore it.

## Software prerequisites

- **nRF Connect SDK v3.4.0** with its toolchain bundle (other versions may work
  but are untested)
- **J-Link** and **nrfutil**, for flashing
- **Python 3.12** for the cloud scripts
- An **AWS account** — the pipeline fits in the free tier for a single bike
- An **nRF Cloud account**, only for A-GNSS assistance (optional; without it the
  GNSS still works, with a longer cold start)

## Getting started

### 1. Firmware configuration

```bash
cp firmware/local.conf.example firmware/local.conf
```

Fill in your AWS IoT endpoint, thing name and APN. `local.conf` is git-ignored.

```bash
aws iot describe-endpoint --endpoint-type iot:Data-ATS
```

### 2. Device credentials

Create an AWS IoT thing, a certificate and a policy, then put the three PEM
files in `firmware/certs/` — see [firmware/certs/README.md](firmware/certs/README.md)
for the exact commands. The policy needs `iot:Connect`, `iot:Publish`,
`iot:Subscribe` and `iot:Receive` on `bike/<thing-name>/*` plus the shadow
topics of that thing.

For A-GNSS, onboard the same board on nRF Cloud so its credentials land in the
nRF Cloud sec_tag. Skip it and set `CONFIG_EBIKE_AGNSS=n` if you do not want it.

### 3. Build and flash

```bash
source env.sh
west build -b nrf9161dk/nrf9161/ns -p always firmware -- -DEXTRA_CONF_FILE=local.conf
cd firmware && ./flash.sh
```

Always flash with `flash.sh` rather than `west flash`. It powers the modem down
cleanly first, which keeps the reset out of the modem's RESET LOOP counter — a
few abrupt resets in a row freeze the LTE attach for 30 minutes, with no reject
cause and nothing visible on the operator side.

The console is on `/dev/ttyACM0` at 115200 baud (the NS application; TF-M logs
on `ttyACM1`).

### 4. Cloud pipeline

```bash
cd cloud
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python provision.py --email you@example.com   # click the SNS confirmation link
.venv/bin/python deploy_alert_notifier.py
.venv/bin/python deploy_console.py
```

`deploy_console.py` prints the public URL and creates
`cloud/secrets/console.json` with a random password on the first run.

To run the console locally instead:

```bash
.venv/bin/python -u dashboard.py    # http://127.0.0.1:8050
```

## Configuration

### Environment variables (cloud)

| Variable | Default | Used by |
|---|---|---|
| `EBIKE_REGION` | `eu-west-3` | all scripts |
| `EBIKE_DEVICE` | `ebike-001` | console, tools — must match `CONFIG_AWS_IOT_CLIENT_ID_STATIC` |
| `EBIKE_TABLE` | `ebike-telemetry` | all scripts |
| `EBIKE_ALERT_TOPIC` | `ebike-alerts` | provisioning, notifier |
| `EBIKE_TZ` | `Europe/Paris` | console, alert e-mails |
| `EBIKE_MAP_CENTER` | `48.8566,2.3522` | console map, before any position is known |

### Firmware options

All under `CONFIG_EBIKE_*` in [firmware/Kconfig](firmware/Kconfig), which
documents the reasoning behind every default. The ones you are most likely to
change:

| Option | Default | What it does |
|---|---|---|
| `WHEEL_CIRC_MM` | 2200 | Wheel circumference; a value pushed from the shadow wins |
| `PUBLISH_RIDING_S` | 60 | Publish rate while riding |
| `PUBLISH_PARKED_S` | 600 | Publish rate while parked |
| `TELEMETRY_SAMPLE_S` | 5 | Trace sampling while riding |
| `TELEMETRY_SAMPLE_PARKED_S` | 60 | Trace sampling while parked |
| `PARK_TIMEOUT_S` | 120 | Stillness before entering PARKED |
| `MOTION_THRESHOLD_MG` | 150 | IMU motion threshold |
| `GEOFENCE_RADIUS_M` | 500 | Default geofence radius |
| `TRACE_RING_SIZE` | 240 | Offline trace depth (32 bytes per sample) |
| `AGNSS` | y | A-GNSS assistance from nRF Cloud |
| `STATUS_LED` | y | Status LEDs on LED1/LED2 (a few mA; disable for endurance runs) |
| `PROVISION_FORCE` | n | Rewrite the AWS credentials at every boot, to rotate them |

### Runtime configuration (Device Shadow)

Writable in `state.desired`, applied by the bike and persisted in NVS:
`armed`, `publish_interval_s`, `motion_threshold_mg`, `geofence_enabled`,
`geofence_radius_m`, `home_lat`, `home_lon`.

`home_lat` and `home_lon` must always be set **together**: the firmware only
applies the pair when both are present in the delta.

The `bike/<id>/cmd` topic carries one-shot commands. Today: `{"set_home":true}`,
which makes the bike's current GNSS position the geofence reference.

## MQTT topics

| Topic | Direction | Payload |
|---|---|---|
| `bike/<id>/status` | up | Periodic status: uptime, RSRP, state, satellites, TTFF, diagnostics |
| `bike/<id>/telemetry` | up | Batched position trace |
| `bike/<id>/alert` | up | Theft and geofence alerts, with position and age |
| `bike/<id>/cmd` | down | One-shot commands |
| `$aws/things/<id>/shadow/*` | both | Configuration and reported state |

## Repository layout

```
env.sh                     activate the NCS toolchain bundle
firmware/
  CMakeLists.txt  Kconfig  prj.conf  local.conf.example
  flash.sh                 graceful flash (modem-safe reset)
  boards/                  devicetree overlay (partitions, IMU, speed sensor)
  dts/bindings/            custom binding for the speed sensor
  certs/                   your device credentials (not versioned)
  src/
    main.c                 boot sequence and SMF state machine
    events/channels.*      zbus channels shared by all modules
    modules/               connectivity, cloud, location, assistance,
                           geofence, sensor, speed, storage, diag, wdt,
                           status_led
cloud/
  provision.py             DynamoDB, SNS and IoT rules
  deploy_alert_notifier.py alert e-mail Lambda
  deploy_console.py        console Lambda plus HTTP API
  dashboard.py charts.py ebike_data.py    the console itself
  show_db.py simbase_cli.py               utilities
docs/ARCHITECTURE.md       how the firmware is put together, and why
```

## Troubleshooting

**The board attaches to LTE and then stops trying.** Look for `RESET LOOP` in the
logs. A few abrupt resets in a row make the modem stop emitting attach requests
for 30 minutes, and the counter persists across power cycles. Leave the board
powered and untouched, and flash with `flash.sh` from then on.

**Nothing on either console after flashing.** The bootloader-less partition
overlay is required. `west flash --recover` erases everything, and only that
layout comes back up afterwards.

**`readback protection enabled` or a TF-M BusFault.** Run
`nrfutil device recover --serial-number <SN>`, then flash again.

**Build fails on `CONFIG_AWS_IOT_BROKER_HOST_NAME is empty`.** You are building
without `local.conf`; see step 1.

**The console shows the bike offline while it is running.** The freshness budget
is derived from the publish rate, so a parked bike is only declared offline
after 25 minutes. Check `qlen` in the status: if it is climbing, the device is
buffering because the link is down.

**On battery, is it even running?** Read the LEDs (see above). Dark LED1 means
the firmware is not running; a blinking LED1 with a dark LED2 means it runs and
the modem never attached. Those are the two failures the dashboard reports
identically.

**`Key access refused` in the boot log.** Expected. The modem will not read a
private key back, so the firmware checks that the credential exists instead of
comparing it, and keeps it. The following line says `present, contents not
readable — kept`. Set `CONFIG_EBIKE_PROVISION_FORCE=y` only to rotate
credentials.

**A slow first fix.** Read `ttff_s` together with `agnss` in the status. A long
TTFF with `ok` is a reception problem; the same TTFF with `fail` points at the
assistance path (credentials, or nRF Cloud onboarding).

## License

Apache-2.0. See [LICENSE](LICENSE).
