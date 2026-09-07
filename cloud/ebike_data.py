#!/usr/bin/env python3
"""eBike data access — DynamoDB plus the Device Shadow. No Dash, no MQTT.

This module is the console's ONLY door to AWS. It is importable and testable on
its own (same intent as charts.py for the plots), and it runs identically
locally and inside the deployment Lambda.

MQTT was deliberately removed. The console used to read live data over
paho-mqtt with mutual TLS using the DEVICE certificates, keeping state in a
thread: neither the persistent connection nor the thread survives in Lambda,
and putting the bike's certificates on a server is not something to do. So
everything goes through DynamoDB polling (the three topics are already
archived there by the IoT rules) plus GetThingShadow. Almost nothing is lost:
the bike publishes at best every 60 s while riding and 600 s while parked, so
polling adds at most DATA_TTL_S. Two benefits come with it — no device secret
on the server side, and no unique MQTT client id to fight over, so several
tabs (or several people) can watch the console at once.

The module cache (`_c`) serves both worlds: a local process living for hours,
or a warm Lambda container reused between requests. It is INCREMENTAL — only
items newer than the last one seen are re-read — so the console tick does not
replay the whole day every time.
"""

import json
import math
import os
import threading
import time
from collections import deque
from datetime import datetime, time as dtime
from decimal import Decimal

import boto3
from boto3.dynamodb.types import TypeDeserializer

REGION = os.environ.get("EBIKE_REGION", "eu-west-3")
DEVICE = os.environ.get("EBIKE_DEVICE", "ebike-001")
TABLE = os.environ.get("EBIKE_TABLE", "ebike-telemetry")

# Cache freshness. The console tick is slower than DATA_TTL_S, so two open tabs
# do not trigger two queries. The shadow changes far less often than telemetry
# (and every GET is billed), hence a wider TTL.
DATA_TTL_S = 8
SHADOW_TTL_S = 25

TRACE_MAX = 5000
ALERTS_MAX = 60

# --- "Trips" tab: how the trace is split and measured ---
TRIP_GAP_S = 180         # gap > 3 min between two points => separate SEGMENT
TRIP_MOVE_KMH = 2.0      # below this, distance is not accumulated (GNSS noise)

# Clients created once: in Lambda this is cold-start work we do not want to
# repeat on every request. The low-level client (rather than `resource`) is used
# because it is thread safe — the local Dash server handles several requests in
# parallel.
_session = boto3.session.Session(region_name=REGION)
_ddb = _session.client("dynamodb")
_iot = _session.client("iot-data")
_deser = TypeDeserializer()


# ------------------------------------------------------------------- utilities
def plain(obj):
    """DynamoDB returns Decimal; convert back to native float/int."""
    if isinstance(obj, Decimal):
        return int(obj) if obj == obj.to_integral_value() else float(obj)
    if isinstance(obj, dict):
        return {k: plain(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [plain(v) for v in obj]
    return obj


def num(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def distance_m(lat1, lon1, lat2, lon2):
    """Equirectangular — same formula as geofence.c, exact below ~50 km."""
    x = math.radians(lon2 - lon1) * math.cos(math.radians((lat1 + lat2) / 2))
    y = math.radians(lat2 - lat1)
    return 6371000.0 * math.hypot(x, y)


def _ms(day, end=False):
    return int(datetime.combine(day, dtime.max if end else dtime.min).timestamp() * 1000)


# -------------------------------------------------------------------- DynamoDB
def query_items(start_ms, end_ms=None):
    """All items of the device within [start_ms, end_ms], paginated, native
    types. Common base for the live view, the trips and the charts."""
    vals = {":d": {"S": DEVICE}, ":a": {"N": str(int(start_ms))}}
    if end_ms is None:
        cond = "device_id = :d AND ts >= :a"
    else:
        cond = "device_id = :d AND ts BETWEEN :a AND :b"
        vals[":b"] = {"N": str(int(end_ms))}

    items, resp, kwargs = [], None, {}
    while resp is None or "LastEvaluatedKey" in resp:
        if resp:
            kwargs["ExclusiveStartKey"] = resp["LastEvaluatedKey"]
        resp = _ddb.query(TableName=TABLE, KeyConditionExpression=cond,
                          ExpressionAttributeValues=vals, **kwargs)
        items.extend(plain({k: _deser.deserialize(v) for k, v in raw.items()})
                     for raw in resp["Items"])
    items.sort(key=lambda i: i["ts"])
    return items


def expand_pts(item):
    """Batch telemetry -> [(ts_ms, spd, acc_m, lat, lon), ...].

    Each point is re-timestamped: the last one lands on the arrival time `ts`
    and the others are shifted by their uptime delta `t`. A batch always belongs
    to one boot session, so `t` is monotonic inside it and reboots are only
    visible between batches.

    The optional `age` (seconds) moves that anchor back. The firmware attaches
    it when it REPLAYS a trace accumulated offline: without it, 20 minutes of
    backlog would pile up at the moment of the catch-up, and successive batches
    — sent back to back — would all land on the same instant. Absent or zero for
    ordinary messages.
    """
    pts = item.get("pts") or []
    if not pts:
        return []
    ts = item["ts"] - (num(item.get("age"), 0) or 0) * 1000.0
    t_last = num(pts[-1].get("t"), 0) or 0
    out = []
    for p in pts:
        dt = (num(p.get("t"), t_last) or t_last) - t_last          # <= 0
        out.append((ts + dt * 1000.0, num(p.get("spd")), num(p.get("acc")),
                    num(p.get("lat")), num(p.get("lon"))))
    return out


def query_history(start_ms):
    """Time window -> (status_rows, gnss), sorted, ready for charts.fig_*()."""
    status_rows, gnss = [], []
    for item in query_items(start_ms):
        if item.get("pts"):
            gnss.extend(expand_pts(item))
        elif item.get("alert"):
            # Alerts also carry `uptime_s`: without this filter they would land
            # in status_rows and punch holes (missing sats/acc) in the curves.
            continue
        elif "uptime_s" in item:
            status_rows.append(item)
    gnss.sort(key=lambda x: x[0])
    status_rows.sort(key=lambda r: r["ts"])
    return status_rows, gnss


def load_trips(day):
    """One local day -> (segments, summary, alerts) for the Trips tab.

    The trace is CUT on gaps longer than TRIP_GAP_S: joining two stretches
    separated by a loss of fix would draw a straight line on the map that the
    bike never rode.

    Distance only accumulates steps whose GNSS speed exceeds TRIP_MOVE_KMH: at
    a standstill, position noise would integrate into phantom kilometres.
    """
    pts, alerts = [], []
    for item in query_items(_ms(day), _ms(day, end=True)):
        if item.get("pts"):
            pts.extend(expand_pts(item))
        elif item.get("alert"):
            alerts.append((datetime.fromtimestamp(item["ts"] / 1000), item))
    pts.sort(key=lambda p: p[0])
    pts = [p for p in pts if p[3] is not None and p[4] is not None]

    segments, cur, prev_ms = [], [], None
    for ms, spd, acc, lat, lon in pts:
        if prev_ms is not None and (ms - prev_ms) / 1000.0 > TRIP_GAP_S:
            if len(cur) > 1:
                segments.append(cur)
            cur = []
        cur.append((ms, spd, acc, lat, lon))
        prev_ms = ms
    if len(cur) > 1:
        segments.append(cur)

    dist_m, moving_s, top = 0.0, 0.0, 0.0
    for seg in segments:
        for a, b in zip(seg, seg[1:]):
            step_m = distance_m(a[3], a[4], b[3], b[4])
            dt_s = (b[0] - a[0]) / 1000.0
            # `spd` is ABSENT when the firmware judged the Doppler speed
            # implausible (see EBIKE_GNSS_MAX_SPEED_KMH). Without a fallback
            # those steps would count as 0 km/h and a whole ride in noisy
            # Doppler would show 0 km travelled. Fall back to the speed derived
            # from positions: less reactive, but honest.
            spd = b[1]
            if spd is None:
                spd = (step_m / dt_s) * 3.6 if dt_s > 0 else 0.0
            top = max(top, spd)
            if spd >= TRIP_MOVE_KMH:
                dist_m += step_m
                moving_s += dt_s
    summary = {
        "dist_km": dist_m / 1000.0,
        "moving_s": moving_s,
        "top_kmh": top,
        "points": len(pts),
        "segments": len(segments),
        "first": datetime.fromtimestamp(pts[0][0] / 1000) if pts else None,
        "last": datetime.fromtimestamp(pts[-1][0] / 1000) if pts else None,
    }
    return segments, summary, sorted(alerts, key=lambda x: x[0])


# ----------------------------------------------------------------- remote control
def set_desired(**fields):
    """Write a command into the Device Shadow. The bike applies it on the next
    delta, or on its periodic shadow re-read (CONFIG_EBIKE_SHADOW_POLL_S)."""
    payload = json.dumps({"state": {"desired": fields}}).encode()
    _iot.update_thing_shadow(thingName=DEVICE, payload=payload)
    now = datetime.now()
    with _lock:
        _c["desired"].update(fields)
        for key in fields:
            _c["desired_ts"][key] = now


def send_cmd(**fields):
    _iot.publish(topic=f"bike/{DEVICE}/cmd", qos=1,
                 payload=json.dumps(fields).encode())


def read_shadow():
    """Full shadow -> (reported, desired, command timestamps).

    Command times come from the AWS metadata rather than from the click: a
    command may have been set by another session, by the CLI, or before this
    process started, and the "waiting" display must stay correct in all those
    cases.
    """
    doc = json.loads(_iot.get_thing_shadow(thingName=DEVICE)["payload"].read())
    st = doc.get("state", {}) or {}
    stamps = {}
    for key, m in ((doc.get("metadata") or {}).get("desired") or {}).items():
        ts = m.get("timestamp") if isinstance(m, dict) else None
        if ts:
            stamps[key] = datetime.fromtimestamp(ts)
    return st.get("reported") or {}, st.get("desired") or {}, stamps


# -------------------------------------------------------- live cache (incremental)
_lock = threading.Lock()
_c = {}


def _reset_day(day):
    """New local day -> start from an empty trace (called under `_lock`)."""
    _c.update({
        "day": day, "last_ts": 0,
        "trace": deque(maxlen=TRACE_MAX), "alerts": deque(maxlen=ALERTS_MAX),
        "status": {}, "status_ts": None,
        "pos": None, "pos_ts": None, "acc_m": None,
    })


with _lock:
    _reset_day(None)
    _c.update({"data_at": 0.0, "shadow_at": 0.0, "aws_ok": False, "error": None,
               "reported": {}, "desired": {}, "desired_ts": {}})


def _refresh_data():
    day = datetime.now().date()
    with _lock:
        if _c["day"] != day:
            _reset_day(day)
        since = _c["last_ts"] or _ms(day) - 1
    try:
        items = query_items(since + 1)
    except Exception as e:                       # AWS unreachable or no rights
        with _lock:
            _c["aws_ok"], _c["error"] = False, str(e)
        print("[db] read failed:", e)
        return

    trace, alerts = [], []
    status = status_ts = pos = pos_ts = acc_m = None
    last = since
    for item in items:
        last = max(last, int(item["ts"]))
        if item.get("pts"):
            for ms, _spd, acc, lat, lon in expand_pts(item):
                if lat is None or lon is None:
                    continue
                trace.append([lat, lon])
                # The anchor is corrected by `age`, so an offline catch-up is
                # NOT mistaken for a fresh position.
                pos, pos_ts, acc_m = [lat, lon], datetime.fromtimestamp(ms / 1000), acc
        elif item.get("alert"):
            alerts.append((datetime.fromtimestamp(item["ts"] / 1000), item))
        elif "uptime_s" in item:
            status, status_ts = item, datetime.fromtimestamp(item["ts"] / 1000)

    with _lock:
        _c["trace"].extend(trace)
        for when, a in alerts:                   # chronological => newest first
            _c["alerts"].appendleft((when, a))
        if status:
            _c["status"], _c["status_ts"] = status, status_ts
        if pos:
            _c["pos"], _c["pos_ts"], _c["acc_m"] = pos, pos_ts, acc_m
        _c["last_ts"] = last
        _c["aws_ok"], _c["error"] = True, None


def _refresh_shadow():
    try:
        reported, desired, stamps = read_shadow()
    except Exception as e:      # best effort: the live read still works
        print("[shadow] GET failed:", e)
        return
    with _lock:
        _c["reported"], _c["desired"] = reported, desired
        _c["desired_ts"].update(stamps)


def snapshot(force=False):
    """Frozen picture of the current state, refreshed according to the TTLs.

    Network I/O happens OUTSIDE the lock, and the freshness counters are set
    before the call so two simultaneous requests do not fetch the same thing
    twice.
    """
    now = time.time()
    with _lock:
        do_data = force or now - _c["data_at"] >= DATA_TTL_S
        do_shadow = force or now - _c["shadow_at"] >= SHADOW_TTL_S
        if do_data:
            _c["data_at"] = now
        if do_shadow:
            _c["shadow_at"] = now
    if do_data:
        _refresh_data()
    if do_shadow:
        _refresh_shadow()
    with _lock:
        return {
            "pos": _c["pos"], "pos_ts": _c["pos_ts"], "acc_m": _c["acc_m"],
            "trace": list(_c["trace"]), "alerts": list(_c["alerts"]),
            "status": dict(_c["status"]), "status_ts": _c["status_ts"],
            "reported": dict(_c["reported"]), "desired": dict(_c["desired"]),
            "desired_ts": dict(_c["desired_ts"]),
            "aws_ok": _c["aws_ok"], "error": _c["error"],
        }


def pending(snap, key):
    """True when the command has not been acknowledged by the bike yet."""
    d, r = snap["desired"], snap["reported"]
    if key not in d:
        return False
    if key not in r:
        return True
    a, b = num(d[key]), num(r[key])
    if a is not None and b is not None:
        return abs(a - b) > 1e-6
    return bool(d[key]) != bool(r[key])
