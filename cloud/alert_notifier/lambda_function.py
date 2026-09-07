"""Lambda turning bike/+/alert messages into readable SNS e-mails (stdlib only).

Triggered by the IoT rule `ebike_alert_notify`
(SQL: SELECT *, topic(2) AS device_id, timestamp() AS ts FROM 'bike/+/alert').
It replaces a raw SNS action, whose body was the bare alert JSON and whose
subject was the generic "AWS Notification Message" for all three causes.

Subject style: a severity tag first, so the mail client can sort and filter on
it.

The SNS SUBJECT must be ASCII (under 100 characters, no line breaks or control
characters), so the subjects below are plain ASCII and _ascii() is the safety
net. The BODY goes out as UTF-8.

Environment: ALERT_TOPIC_ARN (the SNS topic), TZ (local time of the body).
"""

import json
import os
import time
import unicodedata

import boto3

TOPIC_ARN = os.environ["ALERT_TOPIC_ARN"]
sns = boto3.client("sns")

# ASCII subjects, one per cause (1 = motion while armed, 2 = left the zone,
# 3 = back in the zone).
SUBJECTS = {
    1: "[THEFT?] {dev} is moving, alarm active",
    2: "[ZONE EXIT] {dev} left the geofence",
    3: "[OK] {dev} is back in the zone",
}
FOOTER = "\n\n-- Automatic notification from the {dev} system"


def _ascii(s: str) -> str:
    """SNS rejects a non-ASCII subject: strip accents and emoji safely."""
    folded = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    folded = "".join(c for c in folded if c == " " or c.isprintable())
    return folded.strip()[:100]


def _dev(device_id: str) -> str:
    """ebike-001 -> eBike-001 for display (the topic is lowercase)."""
    return device_id.replace("ebike", "eBike")


def _when(ts_ms, age_s=0) -> str:
    """Epoch ms (injected by the rule) -> local time.

    `age_s` is the number of seconds between the moment the bike RAISED the
    alert and its arrival here. It is non-zero when the firmware queued the
    alert for lack of network and replayed it on reconnection. The rule
    timestamp is the ARRIVAL time: without this correction a theft that
    happened twenty minutes earlier would be announced as just having occurred
    — precisely the information that matters when looking for a bike. The delay
    is also stated explicitly, since a corrected time alone would suggest the
    notification was instantaneous.
    """
    try:
        ts = int(ts_ms) / 1000 - float(age_s or 0)
        out = time.strftime("%Y-%m-%d at %H:%M:%S (local time)",
                            time.localtime(ts))
    except (TypeError, ValueError):
        return "just now"

    if age_s and float(age_s) >= 60:
        out += (f"\n                delayed alert: received {_delay(age_s)} "
                "later (bike out of coverage)")
    return out


def _delay(age_s) -> str:
    """Delay in plain words: "3 min", "1 h 12 min"."""
    total = int(float(age_s))
    if total < 3600:
        return f"{total // 60} min"
    return f"{total // 3600} h {(total % 3600) // 60:02d} min"


def _num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _place(event, pad: int = 8) -> str:
    """Position block and map link, only when the triggering fix is attached.

    `acc_m` and `fix_age_s` are optional (cause 1 sends them, the geofence
    causes do not). The fix age is decisive during a theft: a position five
    minutes old does not say where the bike is now, and the mail has to say so.
    """
    lat, lon = _num(event.get("lat")), _num(event.get("lon"))
    if lat is None or lon is None:
        return ""
    k = lambda s: f"  {s.ljust(pad)} : "                       # noqa: E731
    out = (f"{k('Position')}{lat:.6f}, {lon:.6f}\n"
           f"{k('Map')}https://maps.google.com/?q={lat:.6f},{lon:.6f}\n")
    acc = _num(event.get("acc_m"))
    if acc is not None:
        out += f"{k('Accuracy')}+/- {acc:.0f} m\n"
    age = _num(event.get("fix_age_s"))
    if age is not None and age >= 0:
        qual = "fresh position" if age <= 60 else "position already stale"
        out += f"{k('Fix age')}{age:.0f} s ago ({qual})\n"
    return out


def _body_motion(dev, when, event):
    # Aligned on 11 characters, the length of the longest label of the block.
    place = _place(event, pad=11)
    if not place:
        place = ("  Position    : not sent by the bike "
                 "(no valid GNSS fix when the alert was raised)\n")
    return (
        f"THEFT ALERT — {dev}\n\n"
        "Movement was detected on your bike while it is ARMED.\n"
        "This may indicate a theft in progress.\n\n"
        f"  When        : {when}\n"
        "  Trigger     : wheel sensor or IMU\n"
        "  State       : ALARM active\n"
        f"{place}\n"
        "What to do now:\n"
        "  - Open the eBike console to locate the bike live.\n"
        "  - The bike stays in ALARM until it is disarmed remotely.\n"
        "  - A reminder is sent every 60 s while the movement continues."
    )


def _body_geofence_out(dev, when, event):
    dist = _num(event.get("dist_m"))
    dist_line = f"  Distance : {dist:.0f} m from the home point\n" if dist is not None else ""
    return (
        f"ZONE EXIT — {dev}\n\n"
        "Your bike has just left the security zone you defined.\n\n"
        f"  When     : {when}\n"
        f"{dist_line}"
        f"{_place(event)}\n"
        "What to do:\n"
        "  - Check that this move was yours.\n"
        "  - Follow the bike live on the eBike console.\n"
        "  - You will be notified as soon as it is back in the zone."
    )


def _body_geofence_in(dev, when, event):
    dist = _num(event.get("dist_m"))
    dist_line = f"  Distance : {dist:.0f} m from the home point (inside)\n" \
        if dist is not None else ""
    return (
        f"BACK IN ZONE — {dev}\n\n"
        "Good news: your bike is back inside the security zone.\n\n"
        f"  When     : {when}\n"
        f"{dist_line}"
        f"{_place(event)}\n"
        "No action required — monitoring stays active."
    )


BODIES = {1: _body_motion, 2: _body_geofence_out, 3: _body_geofence_in}


def lambda_handler(event, context):
    dev = _dev(str(event.get("device_id", "ebike")))
    # `age_s` absent (the nominal case) means 0, i.e. unchanged behaviour.
    when = _when(event.get("ts"), _num(event.get("age_s")) or 0)
    try:
        cause = int(event.get("cause", 0))
    except (TypeError, ValueError):
        cause = 0

    if cause in BODIES:
        subject = SUBJECTS[cause].format(dev=dev)
        body = BODIES[cause](dev, when, event) + FOOTER.format(dev=dev)
    else:
        # Unknown cause: notify anyway and attach the raw payload, so nothing
        # is lost and a new firmware cause is visible immediately.
        subject = f"[ALERT] {dev} - {event.get('alert', 'event')}"
        body = (f"Uncategorised alert — {dev}\n\n"
                f"  When: {when}\n\n"
                f"Payload received:\n{json.dumps(event, ensure_ascii=False, indent=2)}"
                + FOOTER.format(dev=dev))

    resp = sns.publish(TopicArn=TOPIC_ARN, Subject=_ascii(subject), Message=body)
    return {"messageId": resp["MessageId"], "cause": cause}
