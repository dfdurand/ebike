#!/usr/bin/env python3
"""Charts of the console history tabs.

PURE functions: every `fig_*` takes data already read from DynamoDB and returns
a Plotly figure. No dependency on Dash, boto3 or MQTT, so this file is
importable and testable on its own. Dark theme aligned with dashboard.py.

Input contract (provided by ebike_data.query_history):
- status_rows: list[dict] — one status per cycle (60 s riding, 600 s parked),
               sorted by ascending `ts` (ms). Fields read: ts, spd_wheel,
               rsrp_dbm, sats, sats_used, acc = [x, y, z] (IMU, m/s2).
- gnss:        list[tuple] (ts_ms, spd_kmh, acc_m, lat, lon) — re-timestamped
               GNSS points, sorted by ascending ts.
"""

import math
from datetime import datetime

import plotly.graph_objects as go

# Palette: same hues as the dark theme of dashboard.py.
INK2 = "#c3c2b7"
MUTED = "#898781"
GRID = "rgba(255,255,255,.07)"
GNSS = "#2a78d6"       # speed and GNSS points
WHEEL = "#fab219"      # wheel speed (status)
BLUE = "#3987e5"       # acceleration
GOOD = "#0ca30c"
GEO = "#1baf7a"        # RSRP

AX = "#d03b3b"         # IMU X axis
AY = "#fab219"         # IMU Y axis
AZ = "#1baf7a"         # IMU Z axis

FONT = "system-ui,-apple-system,Segoe UI,sans-serif"
GAP_S = 60             # gap > 60 s between GNSS points => BREAK the curve
GAP_STATUS_S = 900     # same between two status rows: tolerates the 600 s
                       # parked rate but breaks between distinct sessions
G = 9.81


def _num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _dt(ms):
    return datetime.fromtimestamp(ms / 1000.0)


def _layout(title, ytitle, height=260):
    return dict(
        title=dict(text=title, x=0, xanchor="left", font=dict(size=13, color=INK2)),
        height=height, autosize=True,
        margin=dict(l=52, r=18, t=44, b=28),
        paper_bgcolor="rgba(0,0,0,0)", plot_bgcolor="rgba(0,0,0,0)",
        font=dict(color=MUTED, size=12, family=FONT),
        hovermode="x unified",
        legend=dict(orientation="h", y=1.16, x=0, bgcolor="rgba(0,0,0,0)",
                    font=dict(size=11, color=MUTED)),
        xaxis=dict(type="date", gridcolor=GRID, zeroline=False, showline=False,
                   color=MUTED),
        yaxis=dict(title=dict(text=ytitle, font=dict(size=11, color=MUTED)),
                   gridcolor=GRID, zeroline=False, color=MUTED),
    )


def _empty(title):
    fig = go.Figure()
    fig.update_layout(**_layout(title, ""))
    fig.update_xaxes(visible=False)
    fig.update_yaxes(visible=False)
    fig.add_annotation(text="No data for this period", showarrow=False,
                       font=dict(color=MUTED, size=13))
    return fig


def _broken(gnss, idx):
    """(x, y) with a None inserted on gaps longer than GAP_S, so the curve breaks
    instead of joining two stretches across a stop with a straight ramp."""
    xs, ys, prev = [], [], None
    for row in gnss:
        ms, v = row[0], row[idx]
        if prev is not None and (ms - prev) / 1000.0 > GAP_S:
            xs.append(_dt(prev + 1))
            ys.append(None)
        xs.append(_dt(ms))
        ys.append(v)
        prev = ms
    return xs, ys


def fig_speed(status_rows, gnss):
    """Speed: GNSS (solid line) plus the wheel speed of the status (dotted)."""
    if not gnss and not status_rows:
        return _empty("Speed (km/h)")
    fig = go.Figure()
    if gnss:
        xs, ys = _broken(gnss, 1)
        fig.add_trace(go.Scatter(
            x=xs, y=ys, name="GNSS (5 s)", mode="lines", connectgaps=False,
            line=dict(color=GNSS, width=2),
            hovertemplate="%{y:.1f} km/h<extra>GNSS</extra>"))
    wx = [_dt(r["ts"]) for r in status_rows if _num(r.get("spd_wheel")) is not None]
    wy = [_num(r.get("spd_wheel")) for r in status_rows if _num(r.get("spd_wheel")) is not None]
    if wx:
        fig.add_trace(go.Scatter(
            x=wx, y=wy, name="Wheel (status)", mode="lines+markers",
            line=dict(color=WHEEL, width=1.4, dash="dot"), marker=dict(size=5),
            hovertemplate="%{y:.1f} km/h<extra>Wheel</extra>"))
    fig.update_layout(**_layout("Speed (km/h)", "km/h"))
    return fig


def fig_accel(gnss, status_rows):
    """Estimated acceleration = derivative of the GNSS speed: positive means
    accelerating, negative braking. The IMU points (|a| - g, one per publish
    cycle) are overlaid as reference markers rather than a curve, because their
    resolution is far coarser."""
    at, av, prev = [], [], None
    for row in gnss:
        ms, spd = row[0], row[1]
        if prev is not None:
            pms, pspd = prev
            dt = (ms - pms) / 1000.0
            if spd is not None and pspd is not None and 0 < dt <= GAP_S:
                at.append(_dt(ms))
                av.append((spd - pspd) / 3.6 / dt)      # km/h -> m/s, then / s
            else:                                        # gap or stop: break
                at.append(_dt(ms))
                av.append(None)
        prev = (ms, spd)

    imu_t, imu_v = [], []
    for r in status_rows:
        a = r.get("acc")
        if isinstance(a, (list, tuple)) and len(a) == 3:
            comps = [_num(x) for x in a]
            if all(c is not None for c in comps):
                imu_t.append(_dt(r["ts"]))
                imu_v.append(math.sqrt(sum(c * c for c in comps)) - 9.81)

    if not any(v is not None for v in av) and not imu_v:
        return _empty("Estimated acceleration (m/s²)")

    fig = go.Figure()
    fig.add_trace(go.Scatter(
        x=at, y=av, name="a = dV/dt (GNSS)", mode="lines", connectgaps=False,
        line=dict(color=BLUE, width=1.5), fill="tozeroy",
        fillcolor="rgba(57,135,229,.16)",
        hovertemplate="%{y:.2f} m/s²<extra></extra>"))
    if imu_v:
        fig.add_trace(go.Scatter(
            x=imu_t, y=imu_v, name="IMU |a|−g (60 s)", mode="markers",
            marker=dict(size=6, color=MUTED),
            hovertemplate="%{y:.2f} m/s²<extra>IMU</extra>"))
    fig.update_layout(**_layout(
        "Estimated acceleration (m/s²)  ·  + accelerating / − braking", "m/s²"))
    fig.add_hline(y=0, line=dict(color="rgba(255,255,255,.18)", width=1))
    return fig


def fig_imu(status_rows):
    """RAW acceleration measured by the IMU: three axes plus the magnitude.

    Kept separate from fig_accel() because it must be read with two caveats:
    - RESOLUTION: acc[3] only travels in the status, so there is ONE point per
      publish cycle. That shows a trend, not the phases of a ride, hence the
      visible markers rather than a smooth line.
    - NATURE: the sensor sits in an arbitrary orientation and the gyroscope is
      disabled, so the magnitude is about 1 g at rest. What varies usefully here
      is vibration, shocks and tilt — NOT longitudinal acceleration. For
      "accelerating or braking", see fig_accel().
    """
    xs, ax, ay, az, norm, prev = [], [], [], [], [], None
    for r in status_rows:
        a = r.get("acc")
        if not (isinstance(a, (list, tuple)) and len(a) == 3):
            continue
        comps = [_num(v) for v in a]
        if any(c is None for c in comps):
            continue
        ms = r["ts"]
        if prev is not None and (ms - prev) / 1000.0 > GAP_STATUS_S:
            xs.append(_dt(prev + 1))
            for series in (ax, ay, az, norm):
                series.append(None)
        xs.append(_dt(ms))
        ax.append(comps[0])
        ay.append(comps[1])
        az.append(comps[2])
        norm.append(math.sqrt(sum(c * c for c in comps)))
        prev = ms

    if not any(v is not None for v in norm):
        return _empty("Measured acceleration — IMU (m/s²)")

    fig = go.Figure()
    for series, color, name in ((ax, AX, "aX"), (ay, AY, "aY"), (az, AZ, "aZ")):
        fig.add_trace(go.Scatter(
            x=xs, y=series, name=name, mode="lines+markers", connectgaps=False,
            line=dict(color=color, width=1.2), marker=dict(size=3),
            hovertemplate="%{y:.2f} m/s²<extra>" + name + "</extra>"))
    fig.add_trace(go.Scatter(
        x=xs, y=norm, name="|a|", mode="lines+markers", connectgaps=False,
        line=dict(color=BLUE, width=2), marker=dict(size=4),
        hovertemplate="%{y:.2f} m/s²<extra>magnitude</extra>"))
    fig.update_layout(**_layout(
        "Measured acceleration — IMU (m/s²)  ·  1 point per publish cycle",
        "m/s²", height=300))
    fig.add_hline(y=G, line=dict(color="rgba(255,255,255,.22)", width=1, dash="dot"),
                  annotation_text="1 g", annotation_position="top left",
                  annotation_font=dict(size=10, color=MUTED))
    fig.add_hline(y=0, line=dict(color="rgba(255,255,255,.12)", width=1))
    return fig


def fig_signal(status_rows):
    xs = [_dt(r["ts"]) for r in status_rows if _num(r.get("rsrp_dbm")) is not None]
    ys = [_num(r.get("rsrp_dbm")) for r in status_rows if _num(r.get("rsrp_dbm")) is not None]
    if not xs:
        return _empty("Signal LTE — RSRP (dBm)")
    fig = go.Figure(go.Scatter(
        x=xs, y=ys, name="RSRP", mode="lines+markers",
        line=dict(color=GEO, width=2), marker=dict(size=4),
        hovertemplate="%{y:.0f} dBm<extra></extra>"))
    fig.update_layout(**_layout("Signal LTE — RSRP (dBm)", "dBm"))
    return fig


def fig_sats(status_rows):
    xs = [_dt(r["ts"]) for r in status_rows]
    seen = [_num(r.get("sats")) for r in status_rows]
    used = [_num(r.get("sats_used")) for r in status_rows]
    if not xs or not any(v is not None for v in seen + used):
        return _empty("Satellites")
    fig = go.Figure()
    fig.add_trace(go.Scatter(
        x=xs, y=seen, name="seen", mode="lines", line=dict(color=MUTED, width=1.5),
        hovertemplate="%{y:.0f} seen<extra></extra>"))
    if any(v is not None for v in used):
        fig.add_trace(go.Scatter(
            x=xs, y=used, name="used (fix)", mode="lines",
            line=dict(color=GOOD, width=2),
            hovertemplate="%{y:.0f} used<extra></extra>"))
    fig.update_layout(**_layout("Satellites", "count"))
    return fig


def fig_precision(gnss):
    if not gnss or not any(r[2] is not None for r in gnss):
        return _empty("GNSS accuracy (m)")
    xs, ys = _broken(gnss, 2)
    fig = go.Figure(go.Scatter(
        x=xs, y=ys, name="accuracy", mode="lines", connectgaps=False,
        line=dict(color=GNSS, width=1.6), fill="tozeroy",
        fillcolor="rgba(42,120,214,.10)",
        hovertemplate="±%{y:.0f} m<extra></extra>"))
    fig.update_layout(**_layout("GNSS accuracy (m)", "m"))
    return fig
