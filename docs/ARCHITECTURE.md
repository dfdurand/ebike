# Architecture

How the firmware is put together, and why it ended up this way. The per-option
reasoning lives in [firmware/Kconfig](../firmware/Kconfig); this document covers
the shape of the thing.

## Firmware

### Event bus, not call graph

Every module owns exactly one zbus channel and publishes to it at its own
natural rate. No module calls into another.

```
connectivity  -> CH_LTE_STATUS     registration changes
speed         -> CH_SPEED          1 s ticker: wheel speed, odometer
sensor (IMU)  -> CH_MOTION         1 s poll: acceleration, motion flag
location      -> CH_GNSS_FIX       every valid fix, up to ~1 Hz
cloud         -> CH_CLOUD_CMD      message received on bike/<id>/cmd
cloud         -> CH_CONFIG         Device Shadow: armed and configuration
state machine -> CH_ALERT          entering ALARM
geofence      -> CH_ALERT          zone exit or return
```

zbus keeps the last message per channel, which gives two kinds of consumer for
free. **Snapshot** consumers (the telemetry builder) read a channel when they
need it. **Reactive** consumers (the state machine, the alert publisher) are
woken only by publications.

Bulk data deliberately stays off the bus: a channel carries one message, not a
queue, so the batched GNSS trace uses its own ring buffer.

### State machine

```
CONNECT --(cloud connected)--> PARKED <=> RIDING
                                  | (armed + movement)
                                  v
                                ALARM --(remote disarm)--> PARKED
```

- **RIDING** — movement detected; full publish and sampling rate.
- **PARKED** — still for `PARK_TIMEOUT_S`; reduced rates. This exists to spend
  less: fewer LTE wake-ups, more RF windows for GNSS, less cellular data.
- **ALARM** — armed and moving. Immediate alert plus close tracking. Only a
  remote disarm leaves it, because a stolen bike that stopped moving is still
  stolen.

The main thread sleeps on a zbus subscriber and wakes on publications, with a
5 s fallback timeout so the state machine keeps turning even with every sensor
missing.

### Threading

Three contexts, and the split is deliberate.

| Context | What runs there |
|---|---|
| System workqueue | GNSS PVT frames, IMU poll, speed ticker, trace sampling, status LED tick |
| Cloud workqueue | TLS connect, JSON build, publish, alert drain |
| A-GNSS workqueue | The blocking CoAP/DTLS assistance fetch |

The cloud path started on the system workqueue and had to leave it for two
reasons. A slow `aws_iot_connect()` (DNS plus TLS handshake on a degraded
network) blocked PVT processing behind it — exactly during a network recovery,
when GNSS needs it most. And watching the system workqueue with a watchdog
would have required a timeout sized for the worst TLS handshake, which amounts
to watching nothing.

### Failure handling

Four independent mechanisms, because they catch different things:

1. **Fatal errors** — a replacement `k_sys_fatal_error_handler` stores reason,
   PC, LR and thread name in retained RAM before rebooting. Read back and
   published on the next boot.
2. **Silent hangs** — the task watchdog gives one channel per critical loop.
   Short loops get a tight timeout; the cloud queue gets a permissive one, since
   a slow reconnection is legitimate.
3. **Stuck modem resources** — after enough consecutive connection failures
   *with LTE attached*, the board reboots. An abrupt RF loss can leave a TLS
   socket unreleased, after which every connect returns `-ENOMEM` forever and no
   API can force the cleanup. Losing LTE resets the counter, so patchy coverage
   never triggers it.
4. **A board with no console** — on battery the DK's UART is unreachable, so
   two LEDs carry liveness, GNSS freshness and the connection ladder. The
   module polls plain getters rather than subscribing to zbus, because every
   one of them is safe to call before its own module is initialised. That frees
   it from any init-order constraint, which is what lets it start first in
   `main()` — before the long boot steps where a stall would otherwise be
   indistinguishable from a dead board.
5. **Power vs. everything else** — a sentinel in uninitialised RAM distinguishes
   a warm reset from a power loss. This matters because TF-M consumes RESETREAS
   before the non-secure application runs, so the reset cause is often just
   `0x0`. Crossed with the crash record:

   | Boot marker | Crash record | Conclusion |
   |---|---|---|
   | warm | set | application fatal error |
   | warm | empty | watchdog or secure-side fault |
   | cold | — | power supply |

### Offline behaviour

The device is expected to lose the network, so nothing important depends on it
being up.

- **Trace** — samples accumulate in a ring buffer (240 points: 20 minutes
  riding, 4 hours parked). Reads are non-destructive; points are only dropped
  once a publication is accepted, so a send failing after a fragile reconnection
  does not lose what it carried.
- **Alerts** — queued rather than sent inline, and drained on the cloud
  workqueue. Each carries its age, so a replayed alert is not dated at catch-up
  time. When the queue is full the *oldest* is overwritten: in a burst of
  geofence reminders, the newest ones describe better where the bike is.
- **Catch-up** — bounded to a few batches per cycle. Replaying 20 minutes of
  backlog at once would hold the radio for tens of seconds and starve the GNSS.

### GNSS and LTE coexistence

They share one RF path. When the link leaves no observation window, GNSS
priority mode is enabled until the next fix, then released.

Two reporting details exist because the naive version lied about the receiver.
Tracked satellites are the best count over a sliding window rather than the
latest PVT frame — in continuous navigation many epochs carry an empty SV list,
and reporting them showed "0 satellites" in the middle of good reception.
Likewise `sats_used` is only updated on an accepted fix, so it never drops to
zero while the fix age says two seconds.

The plausibility filter separates two failure modes. A fix whose horizontal
accuracy exceeds the threshold is dropped whole. A fix whose Doppler speed is
implausible keeps its **position** and only loses its speed, because position is
what carries the trace.

## Cloud

```
device --MQTT/TLS--> AWS IoT Core
                       |
                       +-- rule -> DynamoDB      (status, telemetry, alerts)
                       +-- rule -> Lambda -> SNS (formatted alert e-mail)
                       |
                     Device Shadow  <---- console (arm, geofence)

browser --HTTPS--> HTTP API --> Lambda (Dash) --> DynamoDB + Device Shadow
```

Everything is provisioned by script, and everything is idempotent.

**No device certificate lives on the server.** The console reads DynamoDB and
the shadow with an IAM role scoped to this one device. That is also why MQTT was
dropped from the console: a persistent connection and its thread do not survive
in Lambda, and the bike's private key has no business on a server.

**Downlink is pulled, not pushed.** A shadow delta is a downlink message, and
with PSM the bike is only reachable in the short window following its own
transmissions, while AWS retransmission backs off exponentially. The device
therefore issues a `shadow/get` during a publication cycle, which bounds how
long a command can sit unread.

**Timestamps are reconstructed.** The firmware has no wall clock and sends
uptimes; the IoT rule injects `device_id` and `ts` at arrival. Batch points are
re-anchored on the last point of the batch, corrected by the `age` the firmware
attaches when replaying a backlog.

## Design decisions worth knowing

| Decision | Why |
|---|---|
| Bootloader-less partition layout | `west flash --recover` is mandatory (APPROTECT re-locks every boot) and erases everything; only this layout reboots afterwards |
| Graceful modem power-down before flashing | The modem RESET LOOP counter freezes the attach for 30 minutes after a few abrupt resets |
| Equirectangular distance | Accurate well beyond geofence scale, cheaper than haversine |
| Geofence independent of `armed` | Arming already alerts on any movement; the geofence covers a lent bike, or an owner who forgot to arm |
| Both a spatial and a temporal geofence debounce | Hysteresis alone still flaps for a bike parked exactly on the boundary |
| Credential provisioning checks existence, not contents | The modem refuses to read a private key back, so a comparison can never succeed and would rewrite the NVM on every boot |
| Identical shadow reports are dropped | The same payload went out twice at every boot and transition — direct, then deferred — for no settlement on the AWS side |
| Provisioned DynamoDB capacity | Provisioned 5/5 is in the permanent free tier; on-demand is billed per request |
| HTTP API instead of a Lambda Function URL | Some accounts refuse anonymous Function URL invocation, and no SDK call lifts that guard |
