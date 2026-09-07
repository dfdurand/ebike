#!/usr/bin/env python3
"""eBike console — live map and anti-theft remote. Pure Python.

Interface only: all AWS reads and writes live in ebike_data.py, all charts in
charts.py. The same app runs locally (python -u dashboard.py) and behind the
deployment Lambda (console/lambda_function.py imports `server`), so there are
not two versions to maintain.

Three tabs. Hidden panels stay mounted — a plain CSS toggle — otherwise the
callbacks that target their components fail.
- Live:    real-time map, status tiles, alerts, remote control.
- Trips:   one day out of the last seven, drawn as a path on the map.
- Stats:   speed, acceleration, IMU, RSRP, satellites and accuracy charts.

Reading: DynamoDB and Device Shadow polling (ebike_data.snapshot()) every
TICK_MS. MQTT and the device certificates were removed when the console moved
to AWS — see the header of ebike_data.py. A pleasant consequence: no unique
client id to fight over, so the console can be open in several tabs at once.

Writing (remote control):
- Lock / unlock  -> Device Shadow desired.armed
- Geofence       -> desired.geofence_enabled / geofence_radius_m / home_lat+lon
- "Set home here" -> publishes {"set_home":true} on bike/<id>/cmd, and the bike
  uses its own fix

home_lat and home_lon are ALWAYS set together: the firmware only applies the
pair when both are present in the delta (see cloud.c).

Usage:  .venv/bin/python -u dashboard.py   then http://127.0.0.1:8050

Environment:
  EBIKE_MAP_CENTER  "lat,lon" used before any position is known
"""

import os
from datetime import datetime, time as dtime, timedelta

import dash
import dash_leaflet as dl
from dash import dcc, html, Input, Output, State, ctx, no_update
from dash.exceptions import PreventUpdate

import charts
import ebike_data as data
from ebike_data import DEVICE, distance_m, num

# Map center used until a position is known. Falls back to a neutral location;
# override with EBIKE_MAP_CENTER="lat,lon".
try:
    DEFAULT_CENTER = [float(x) for x in
                      os.environ.get("EBIKE_MAP_CENTER", "48.8566,2.3522").split(",")]
except ValueError:
    DEFAULT_CENTER = [48.8566, 2.3522]
WHEEL_CIRC_M = 2.2                       # CONFIG_EBIKE_WHEEL_CIRC_MM = 2200
MAX_ZOOM = 21                            # see maxNativeZoom of the tile layer

# Refresh rate of the live view. Under polling every tick costs a request (and a
# Lambda invocation once deployed), and 10 s stays well below the publish rate of
# the bike (60 s while riding), so nothing is actually missed.
TICK_MS = 10_000

# --- Trips tab ---
TRIP_DAYS = 7            # depth of the day selector
DAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

# Freshness: the bike publishes a status on EVERY cycle (60 s riding, 600 s
# parked). Past that silence, times a margin, it is declared offline rather than
# shown with a stale state. Riding and alarm use a tight rate, so detection is
# fast; parked is tolerant, since the bike sleeps in PSM.
FRESH_RIDING_S = 180     # 3x the 60 s rate
FRESH_PARKED_S = 1500    # 2.5x the 600 s rate

# Expected wait for a DOWNLINK command (arming, geofence), aligned with the
# firmware. A shadow delta is NOT pushed to the bike on demand: with PSM it is
# only reachable in the window following its own transmissions. Since
# CONFIG_EBIKE_SHADOW_POLL_S the bike re-reads the shadow itself during a
# publication cycle, so the wait is bounded by the parked cycle and by the
# re-read period while riding.
SHADOW_POLL_S = 300      # CONFIG_EBIKE_SHADOW_POLL_S
PUBLISH_PARKED_S = 600   # CONFIG_EBIKE_PUBLISH_PARKED_S

# Palette. The UI is dark but the MAP stays on light OSM tiles, so the marks
# drawn on it use the light steps of the same hues.
C_TRACE = "#2a78d6"      # GNSS trace
C_GEO = "#1baf7a"        # active geofence
C_MUTED = "#898781"
C_GOOD = "#0ca30c"
C_WARN = "#fab219"
C_CRIT = "#d03b3b"

ALERTS = {
    1: ("Motion detected — bike locked", C_CRIT, "🚨"),
    2: ("Left the zone", C_CRIT, "📍"),
    3: ("Back inside the zone", C_GOOD, "✅"),
}

# `reset` comes from diag_reset_cause_str(): 0x0 means the SoC left the flags
# unset, which happens when TF-M consumed RESETREAS first.
RESETS = {"0x0": "—", "None": "—", "por": "power-on", "pin": "button",
          "brownout": "brown-out", "software": "software", "watchdog": "watchdog"}


# ----------------------------------------------------------------- presentation
def ago(ts):
    if ts is None:
        return "—"
    s = int((datetime.now() - ts).total_seconds())
    if s < 60:
        return f"{s} s ago"
    if s < 3600:
        return f"{s // 60} min ago"
    return f"{s // 3600} h {(s % 3600) // 60:02d} ago"


def hms(seconds):
    s = int(num(seconds, 0) or 0)
    if s < 3600:
        return f"{s // 60} min"
    return f"{s // 3600} h {(s % 3600) // 60:02d}"


def wait_badge(since, sm, online):
    """Waiting badge for a command: how long it has been AND how long it is
    supposed to take.

    Without that bound the user cannot tell NORMAL latency (the bike sleeps in
    PSM and is only reachable on its own wake-ups) from a real problem — ten
    minutes of silence look identical in both cases.

    `since` is when the command was written, read from the shadow metadata
    rather than from the click, because it may have come from elsewhere.
    """
    budget = PUBLISH_PARKED_S if sm == "parked" else SHADOW_POLL_S
    base = "⏳ waiting for the bike"

    if not online:
        return f"{base} · bike offline, the command will go out on reconnect", "wait"
    if since is None:
        return f"{base} · check-in ≤ {hms(budget)}", "wait"

    el = int((datetime.now() - since).total_seconds())
    el_txt = f"{el} s" if el < 60 else hms(el)
    if el > 2 * budget:
        return (f"{base} · {el_txt} — unusually long "
                f"(check-in expected ≤ {hms(budget)})"), "wait late"
    return (f"{base} · {el_txt} / check-in ≤ {hms(budget)} "
            f"({'parked' if sm == 'parked' else 'biking'})"), "wait"


def day_label(d, today):
    """Date -> short label. strftime names follow the system locale, which is
    not guaranteed, so they are not relied on."""
    delta = (today - d).days
    if delta == 0:
        return "Today"
    if delta == 1:
        return "Yesterday"
    return f"{DAYS[d.weekday()]} {MONTHS[d.month - 1]} {d.day}"


def day_options(today=None):
    """The last TRIP_DAYS days, most recent first."""
    today = today or datetime.now().date()
    days = [today - timedelta(days=i) for i in range(TRIP_DAYS)]
    return [{"label": day_label(d, today), "value": d.isoformat()} for d in days]

# ------------------------------------------------------------------------- style
CSS = """
:root{
  --plane:#0d0d0d; --surface:#1a1a19; --raised:#232321;
  --ink:#ffffff; --ink2:#c3c2b7; --muted:#898781;
  --line:rgba(255,255,255,.10);
  --good:#0ca30c; --warn:#fab219; --crit:#d03b3b; --trace:#3987e5;
  --r:16px;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{
  background:var(--plane); color:var(--ink);
  font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;
  -webkit-font-smoothing:antialiased;
  background-image:
    radial-gradient(900px 500px at 15% -10%, rgba(57,135,229,.10), transparent 60%),
    radial-gradient(700px 400px at 95% 0%, rgba(25,158,112,.07), transparent 60%);
  background-attachment:fixed;
}
.wrap{max-width:1280px;margin:0 auto;padding:18px 16px 40px}

/* ---- header ---- */
header{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:18px}
.logo{font-size:22px;font-weight:650;letter-spacing:-.02em}
.logo small{display:block;font-size:12px;font-weight:400;color:var(--muted);letter-spacing:.06em}
.chips{display:flex;gap:8px;flex-wrap:wrap;margin-left:auto}
.chip{display:inline-flex;align-items:center;gap:7px;padding:6px 12px;border-radius:999px;
  background:var(--surface);border:1px solid var(--line);font-size:13px;color:var(--ink2)}
.chip b{color:var(--ink);font-weight:600}
.dot{width:8px;height:8px;border-radius:50%;background:var(--muted);flex:none}
.dot.on{background:var(--good);box-shadow:0 0 0 3px rgba(12,163,12,.18)}
.dot.off{background:var(--crit);box-shadow:0 0 0 3px rgba(208,59,59,.18)}

/* ---- grid ---- */
.grid{display:grid;grid-template-columns:380px 1fr;gap:16px;align-items:start}
@media(max-width:900px){.grid{grid-template-columns:1fr}}
.col{display:flex;flex-direction:column;gap:16px}

.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r);padding:18px}
.card h2{margin:0 0 14px;font-size:12px;font-weight:600;letter-spacing:.10em;
  text-transform:uppercase;color:var(--muted)}
.card h2 .side{float:right;letter-spacing:0;text-transform:none;font-weight:400}

/* ---- anti-theft remote ---- */
.seg{display:grid;grid-template-columns:1fr 1fr;gap:8px;
  background:var(--plane);border:1px solid var(--line);border-radius:14px;padding:6px}
.seg button{
  appearance:none;border:0;cursor:pointer;border-radius:10px;padding:16px 10px;
  font:600 15px/1.2 system-ui,-apple-system,"Segoe UI",sans-serif;letter-spacing:.02em;
  background:transparent;color:var(--ink2);transition:background .15s,color .15s,box-shadow .15s}
.seg button:hover{background:rgba(255,255,255,.05);color:var(--ink)}
.seg button .sub{display:block;font-size:11px;font-weight:400;color:var(--muted);margin-top:3px}
.seg button.on{color:#0d0d0d;background:var(--ink2)}
.seg button.on .sub{color:rgba(13,13,13,.65)}
.seg button.on.armed{background:var(--warn);box-shadow:0 0 24px rgba(250,178,25,.28)}
.seg button.on.alarm{background:var(--crit);color:#fff;box-shadow:0 0 26px rgba(208,59,59,.40);
  animation:pulse 1.4s ease-in-out infinite}
.seg button.on.alarm .sub{color:rgba(255,255,255,.75)}
@keyframes pulse{50%{box-shadow:0 0 6px rgba(208,59,59,.15)}}

.state{margin-top:14px;display:flex;align-items:center;gap:10px;font-size:14px;color:var(--ink2)}
.state .big{font-size:15px;font-weight:600;color:var(--ink)}
.wait{font-size:12px;color:var(--warn);border:1px solid rgba(250,178,25,.35);
  background:rgba(250,178,25,.10);border-radius:999px;padding:3px 10px}
/* wait well past the expected listening window */
.wait.late{color:#f0a3a3;border-color:rgba(208,59,59,.45);
  background:rgba(208,59,59,.10)}

/* ---- fields ---- */
.row{display:flex;gap:10px;align-items:flex-end;flex-wrap:wrap;margin-bottom:12px}
.field{flex:1 1 120px;min-width:0}
.field label{display:block;font-size:11px;letter-spacing:.06em;text-transform:uppercase;
  color:var(--muted);margin-bottom:5px}
.field input{width:100%;background:var(--plane);color:var(--ink);border:1px solid var(--line);
  border-radius:10px;padding:10px 12px;font:14px/1.2 ui-monospace,SFMono-Regular,Menlo,monospace;
  font-variant-numeric:tabular-nums;-moz-appearance:textfield}
.field input::-webkit-outer-spin-button,
.field input::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
.field input:focus{outline:none;border-color:rgba(57,135,229,.65);
  box-shadow:0 0 0 3px rgba(57,135,229,.15)}
.btn{appearance:none;cursor:pointer;border:1px solid var(--line);background:var(--raised);
  color:var(--ink);border-radius:10px;padding:10px 14px;
  font:500 14px/1.2 system-ui,-apple-system,"Segoe UI",sans-serif;
  transition:background .15s,border-color .15s}
.btn:hover{background:#2c2c29;border-color:rgba(255,255,255,.20)}
.btn.primary{background:var(--trace);border-color:transparent;color:#fff;font-weight:600}
.btn.primary:hover{background:#4a93ea}
.btn.wide{width:100%;justify-content:center}
.hint{font-size:12px;color:var(--muted);margin:2px 0 0}
.hint .off{color:var(--warn);font-weight:600}

/* ---- tiles ---- */
.tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:1px;
  background:var(--line);border:1px solid var(--line);border-radius:var(--r);overflow:hidden}
@media(max-width:560px){.tiles{grid-template-columns:repeat(2,1fr)}}
.tiles.stale{opacity:.45;filter:grayscale(.4)}  /* bike offline: stale values */
.tile{background:var(--surface);padding:14px 16px}
.tile .k{font-size:10px;letter-spacing:.10em;text-transform:uppercase;color:var(--muted)}
.tile .v{font-size:24px;font-weight:600;letter-spacing:-.02em;margin-top:4px;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tile .v span{font-size:12px;font-weight:400;color:var(--muted);margin-left:3px}

/* ---- map ---- */
.mapbox{position:relative;border:1px solid var(--line);border-radius:var(--r);overflow:hidden}
.mapbox .float{position:absolute;z-index:500;right:12px;top:12px}
/* Light map background (OSM tiles): the controls keep the app colours, which
   read well against the tiles, while the attribution blends in. */
.leaflet-container{background:#e8e6e1}
.leaflet-control-zoom a{background:var(--raised)!important;color:var(--ink2)!important;
  border-color:rgba(255,255,255,.14)!important}
.leaflet-control-zoom a:hover{background:#2c2c29!important;color:#fff!important}
.leaflet-control-attribution{background:rgba(255,255,255,.78)!important;color:#52514e!important}
.leaflet-control-attribution a{color:#52514e!important}

/* ---- alertes ---- */
.alert{display:flex;gap:12px;padding:11px 0;border-top:1px solid var(--line)}
.alert:first-child{border-top:0;padding-top:0}
.alert .ic{font-size:16px;line-height:1.3}
.alert .t{font-size:12px;color:var(--muted);font-variant-numeric:tabular-nums}
.alert .m{font-weight:550;font-size:14px}
.alert .d{font-size:12px;color:var(--muted);font-family:ui-monospace,Menlo,monospace}
.empty{color:var(--muted);font-size:13px}

.toast{margin-top:12px;font-size:13px;padding:10px 12px;border-radius:10px;
  border:1px solid var(--line);background:var(--plane);color:var(--ink2)}
.toast.err{border-color:rgba(208,59,59,.45);color:#f0a3a3}
.toast.ok{border-color:rgba(12,163,12,.40);color:#96d996}

/* ---- onglets (Live / Trajets / Statistiques) ---- */
.tabs .tab:hover{color:var(--ink)!important}

/* ---- barre de filtres (Trajets + Statistiques) ---- */
.hist-bar{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:16px}
.rng{display:flex;gap:6px;flex-wrap:wrap}
.rng label{display:inline-flex;align-items:center;gap:6px;font-size:13px;color:var(--ink2);
  background:var(--surface);border:1px solid var(--line);border-radius:999px;
  padding:6px 12px;cursor:pointer}
.rng input{accent-color:var(--trace);margin:0}
.hist-meta{font-size:12px;color:var(--muted);margin-left:auto;
  font-variant-numeric:tabular-nums}

/* ---- grille de graphes ---- */
.charts{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:900px){.charts{grid-template-columns:1fr}}
.charts .card{padding:12px 12px 6px}
.charts .card.w2{grid-column:1 / -1}
"""

INDEX = """<!DOCTYPE html>
<html lang="en">
<head>
{%metas%}
<title>{%title%}</title>
{%favicon%}
{%css%}
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<style>""" + CSS + """</style>
</head>
<body>{%app_entry%}<footer>{%config%}{%scripts%}{%renderer%}</footer></body>
</html>"""


# Tabs and charts (dark theme; figures live in charts.py).
TAB = {"backgroundColor": "transparent", "color": "#898781", "border": "0",
       "borderBottom": "2px solid transparent", "padding": "10px 18px",
       "fontWeight": "600", "fontFamily": "inherit"}
TAB_SEL = {**TAB, "color": "#ffffff", "borderBottom": "2px solid #3987e5"}
GRAPH_CFG = {"displaylogo": False, "responsive": True,
             "modeBarButtonsToRemove": ["select2d", "lasso2d", "autoScale2d"]}
GRAPH_STYLE = {"height": "260px"}


# ---------------------------------------------------------------------- layout
def tile(k, v, unit=None):
    return html.Div(className="tile", children=[
        html.Div(k, className="k"),
        html.Div([str(v), html.Span(unit)] if unit else str(v), className="v")])


def field(id_, label, value, **kw):
    """Text field with a decimal keypad. type=number forces the native Chrome
    spinners, which eat the usable width (lat/lon are nine characters)."""
    return html.Div(className="field", children=[
        html.Label(label),
        dcc.Input(id=id_, type="text", inputMode="decimal",
                  value="" if value is None else str(value), **kw)])


def parse(v):
    """User input -> float, accepting a decimal comma."""
    try:
        return float(str(v).strip().replace(",", "."))
    except (TypeError, ValueError):
        return None


def serve_layout():
    snap = data.snapshot()
    rep = snap["reported"]
    center = snap["pos"] or [num(rep.get("home_lat")) or DEFAULT_CENTER[0],
                             num(rep.get("home_lon")) or DEFAULT_CENTER[1]]
    return html.Div(className="wrap", children=[
        html.Header([
            html.Div(className="logo", children=[
                "🚲 eBike", html.Small(DEVICE.upper())]),
            html.Div(id="chips", className="chips"),
        ]),
        dcc.Tabs(id="tabs", value="live", className="tabs",
                 style={"borderBottom": "1px solid var(--line)", "marginBottom": "18px"},
                 children=[
                     dcc.Tab(label="Live", value="live", style=TAB, selected_style=TAB_SEL),
                     dcc.Tab(label="Trips", value="trips",
                             style=TAB, selected_style=TAB_SEL),
                     dcc.Tab(label="Statistics", value="stats",
                             style=TAB, selected_style=TAB_SEL),
                 ]),
        html.Div(id="panel-live", children=html.Div(className="grid", children=[
            html.Div(className="col", children=[
                # ---- anti-theft
                html.Div(className="card", children=[
                    html.H2("Anti-theft"),
                    html.Div(className="seg", children=[
                        html.Button(id="btn-disarm", n_clicks=0, children=[
                            "Unlocked", html.Span("monitoring off", className="sub")]),
                        html.Button(id="btn-arm", n_clicks=0, children=[
                            "Locked", html.Span("alerts on the slightest movement",
                                                className="sub")]),
                    ]),
                    html.Div(id="arm-state", className="state"),
                    html.Div(id="toast", className="toast",
                             children="Ready. Locking takes effect as soon as the bike "
                                      "receives the delta (immediate if it is online)."),
                ]),
                # ---- geofence
                html.Div(className="card", children=[
                    html.H2("Geofence"),
                    html.Button("📍 Set home point here", id="btn-home-here",
                                n_clicks=0, className="btn wide"),
                    html.P("The bike takes its current GNSS position (a valid fix is "
                           "required) and saves it to the shadow.", className="hint"),
                    html.Div(className="row", style={"marginTop": "16px"}, children=[
                        field("in-lat", "Latitude", num(rep.get("home_lat")),
                              placeholder="48.856600"),
                        field("in-lon", "Longitude", num(rep.get("home_lon")),
                              placeholder="2.352200"),
                    ]),
                    html.Div(className="row", children=[
                        field("in-radius", "Radius (m)",
                              int(num(rep.get("geofence_radius_m"), 200) or 200)),
                    ]),
                    html.Button("Apply home point + radius", id="btn-apply", n_clicks=0,
                                className="btn primary wide"),
                    html.Button(id="btn-geo", n_clicks=0, className="btn wide",
                                style={"marginTop": "8px"}),
                    html.Div(id="geo-info", className="hint", style={"marginTop": "10px"}),
                ]),
            ]),
            html.Div(className="col", children=[
                html.Div(id="tiles", className="tiles"),
                html.Div(className="mapbox", children=[
                    html.Div(className="float", children=html.Button(
                        "◎ Recenter", id="btn-center", n_clicks=0, className="btn")),
                    dl.Map(id="map", center=center, zoom=15, maxZoom=MAX_ZOOM,
                           style={"height": "clamp(340px,52vh,600px)"},
                           children=[
                               # OSM only serves tiles up to z19
                               # (maxNativeZoom); beyond that Leaflet scales the
                               # z19 tile up. Slightly blurry, but it shows the
                               # exact spot the bike passed.
                               dl.TileLayer(
                                   url="https://{s}.tile.openstreetmap.org/"
                                       "{z}/{x}/{y}.png",
                                   attribution="© OpenStreetMap",
                                   maxZoom=MAX_ZOOM, maxNativeZoom=19),
                               dl.LayerGroup(id="layers"),
                           ]),
                ]),
                html.Div(className="card", children=[
                    html.H2(["Alerts", html.Span(id="alert-count", className="side")]),
                    html.Div(id="alerts"),
                ]),
            ]),
        ])),
        # ---- Trips tab: one day drawn as a path on the map
        html.Div(id="panel-trips", style={"display": "none"}, children=[
            html.Div(className="hist-bar", children=[
                dcc.RadioItems(id="trip-day", className="rng", inline=True,
                               value=datetime.now().date().isoformat(),
                               options=day_options()),
                html.Button("↻ Reload", id="trip-reload", n_clicks=0,
                            className="btn"),
                html.Span(id="trip-meta", className="hist-meta"),
            ]),
            html.Div(id="trip-tiles", className="tiles"),
            html.Div(className="mapbox", style={"marginTop": "16px"}, children=[
                dl.Map(id="trip-map", center=DEFAULT_CENTER, zoom=14,
                       maxZoom=MAX_ZOOM,
                       style={"height": "clamp(380px,60vh,720px)"},
                       children=[
                           dl.TileLayer(
                               url="https://{s}.tile.openstreetmap.org/"
                                   "{z}/{x}/{y}.png",
                               attribution="© OpenStreetMap",
                               maxZoom=MAX_ZOOM, maxNativeZoom=19),
                           dl.LayerGroup(id="trip-layers"),
                       ]),
            ]),
        ]),
        # ---- Stats tab
        html.Div(id="panel-stats", style={"display": "none"}, children=[
            html.Div(className="hist-bar", children=[
                dcc.RadioItems(
                    id="hist-range", value="today", className="rng", inline=True,
                    options=[{"label": "Today", "value": "today"},
                             {"label": "24 h", "value": "24h"},
                             {"label": "7 days", "value": "7d"}]),
                html.Button("↻ Reload", id="hist-reload", n_clicks=0, className="btn"),
                html.Span(id="hist-meta", className="hist-meta"),
            ]),
            html.Div(className="charts", children=[
                html.Div(className="card w2", children=dcc.Graph(
                    id="g-speed", config=GRAPH_CFG, style=GRAPH_STYLE)),
                html.Div(className="card w2", children=dcc.Graph(
                    id="g-accel", config=GRAPH_CFG, style=GRAPH_STYLE)),
                html.Div(className="card w2", children=dcc.Graph(
                    id="g-imu", config=GRAPH_CFG, style=GRAPH_STYLE)),
                html.Div(className="card", children=dcc.Graph(
                    id="g-signal", config=GRAPH_CFG, style=GRAPH_STYLE)),
                html.Div(className="card", children=dcc.Graph(
                    id="g-sats", config=GRAPH_CFG, style=GRAPH_STYLE)),
                html.Div(className="card w2", children=dcc.Graph(
                    id="g-precision", config=GRAPH_CFG, style=GRAPH_STYLE)),
            ]),
        ]),
        dcc.Interval(id="tick", interval=TICK_MS),
    ])


# compress=True is not cosmetic in Lambda: a response is capped at 6 MB, and the
# plotly.min.js bundle served on the first visit is 4.85 MB, i.e. 6.5 MB once
# base64 encoded — over the cap, and the page stays blank. Gzipped it drops to
# about 1.4 MB. Locally it only saves bandwidth.
app = dash.Dash(__name__, title="eBike · console", update_title=None, compress=True)
app.index_string = INDEX
app.layout = serve_layout

# WSGI entry point: console/lambda_function.py calls it directly, and gunicorn
# would use it the same way if the console were hosted differently.
server = app.server


# ------------------------------------------------------------------- callbacks
@app.callback(Output("toast", "children"), Output("toast", "className"),
              Input("btn-arm", "n_clicks"), Input("btn-disarm", "n_clicks"),
              Input("btn-home-here", "n_clicks"), Input("btn-apply", "n_clicks"),
              Input("btn-geo", "n_clicks"),
              State("in-lat", "value"), State("in-lon", "value"),
              State("in-radius", "value"),
              prevent_initial_call=True)
def act(_a, _d, _h, _p, _g, lat, lon, radius):
    who, t = ctx.triggered_id, datetime.now().strftime("%H:%M:%S")
    try:
        if who == "btn-arm":
            data.set_desired(armed=True)
            msg = "🔒 Lock requested — ALARM on the first motion detected."
        elif who == "btn-disarm":
            data.set_desired(armed=False)
            msg = "🔓 Unlock requested — the only way out of the ALARM state."
        elif who == "btn-home-here":
            data.send_cmd(set_home=True)
            msg = ("📍 set_home sent — the bike applies its current position if it "
                   "has a valid GNSS fix (otherwise the command is ignored).")
        elif who == "btn-apply":
            la, lo, r = parse(lat), parse(lon), parse(radius)
            if la is None or lo is None:
                return f"{t} — valid latitude and longitude required (both).", "toast err"
            # The firmware only applies the pair when both fields are in the
            # delta, so they are always set together (see cloud.c).
            fields = {"home_lat": la, "home_lon": lo}
            if r and r > 0:
                fields["geofence_radius_m"] = int(r)
            data.set_desired(**fields)
            msg = (f"Home point {la:.6f}, {lo:.6f}"
                   + (f" · radius {int(r)} m" if r else "") + " sent.")
            # Setting a home point does NOT re-enable monitoring:
            # geofence_enabled is only touched by its own button. Re-applying
            # the radius feels like the natural way to switch the geofence back
            # on, and it is not, so say so.
            if not bool(data.snapshot()["reported"].get("geofence_enabled")):
                msg += (" ⚠️ Geofence DISABLED: no alert will fire until you click "
                        "“Enable geofence”.")
        elif who == "btn-geo":
            new = not bool(data.snapshot()["reported"].get("geofence_enabled"))
            data.set_desired(geofence_enabled=new)
            msg = f"Geofence {'enabled' if new else 'disabled'} — command sent."
        else:
            return no_update, no_update
    except Exception as e:
        return f"{t} — failed: {e}", "toast err"
    return f"{t} — {msg}", "toast ok"


@app.callback(Output("map", "center"), Output("map", "zoom"),
              Input("btn-center", "n_clicks"), prevent_initial_call=True)
def recenter(_):
    snap = data.snapshot()
    pos = snap["pos"]
    home = [num(snap["reported"].get("home_lat")),
            num(snap["reported"].get("home_lon"))]
    target = pos or (home if home[0] is not None else DEFAULT_CENTER)
    return target, 17


@app.callback(
    Output("chips", "children"), Output("tiles", "children"),
    Output("tiles", "className"),
    Output("layers", "children"), Output("alerts", "children"),
    Output("alert-count", "children"),
    Output("btn-arm", "className"), Output("btn-disarm", "className"),
    Output("arm-state", "children"), Output("btn-geo", "children"),
    Output("geo-info", "children"),
    Input("tick", "n_intervals"))
def refresh(_):
    snap = data.snapshot()
    pos, pos_ts, acc = snap["pos"], snap["pos_ts"], snap["acc_m"]
    trace, alerts = snap["trace"], snap["alerts"]
    st, rep, st_ts = snap["status"], snap["reported"], snap["status_ts"]
    aws_ok = snap["aws_ok"]
    wait_armed = data.pending(snap, "armed")
    wait_geo = data.pending(snap, "geofence_enabled")
    armed_since = snap["desired_ts"].get("armed")

    sm = str(rep.get("sm_state", "?"))
    armed, geo_on = bool(rep.get("armed")), bool(rep.get("geofence_enabled"))

    # -- liveness of the BIKE (not of the console link to AWS)
    last_ts = max((t for t in (st_ts, pos_ts) if t), default=None)
    budget = FRESH_RIDING_S if sm in ("riding", "alarm") else FRESH_PARKED_S
    fresh = last_ts is not None and (datetime.now() - last_ts).total_seconds() < budget
    online = aws_ok and fresh           # the bike is genuinely reachable
    alarm = sm == "alarm"
    alarm_live = alarm and online       # only blink on a live-confirmed alarm

    # -- header chips
    if not aws_ok:                      # the console itself cannot reach AWS
        conn_dot, conn_txt = "off", "console offline"
    elif not fresh:                    # AWS is fine but the bike is silent
        conn_dot, conn_txt = "off", "bike offline"
    else:
        conn_dot, conn_txt = "on", "up to date"

    sm_label = ("📴 Offline" if not online else
                {"parked": "🅿 Parked", "riding": "🚴 Biking",
                 "alarm": "🚨 ALARM", "connect": "⏳ Connecting"}.get(sm, f"state {sm}"))
    chips = [
        html.Span(className="chip", children=[
            html.Span(className=f"dot {conn_dot}"), conn_txt]),
        html.Span(className="chip", children=html.B(sm_label)),
        html.Span(className="chip", children=[
            html.Span(className="dot", style={"background": C_WARN if armed else C_MUTED}),
            html.B("Locked" if armed else "Unlocked")]),
        html.Span(className="chip", children=f"bike seen {ago(last_ts)}"),
    ]

    # -- tiles
    revs = num(st.get("revs"), 0) or 0
    tiles = [
        tile("Wheel speed", f"{num(st.get('spd_wheel'), 0):.1f}", " km/h"),
        tile("Satellites", f"{int(num(st.get('sats_used'), 0) or 0)}",
             f" / {int(num(st.get('sats'), 0) or 0)} seen"),
        tile("Signal", f"{int(num(st.get('rsrp_dbm'), 0) or 0)}", " dBm"),
        tile("Accuracy", f"{acc:.0f}" if acc else "—", " m"),
        tile("Fix age", f"{int(num(st.get('fix_age_s'), 0) or 0)}", " s"),
        tile("Odometer", f"{revs * WHEEL_CIRC_M / 1000:.2f}", " km"),
        tile("Uptime", hms(st.get("uptime_s"))),
        tile("Last reset", RESETS.get(str(st.get("reset")), str(st.get("reset", "—")))),
    ]

    # -- map layers
    layers = []
    if len(trace) > 1:
        layers.append(dl.Polyline(positions=trace, color=C_TRACE, weight=3, opacity=0.85))
    hlat, hlon = num(rep.get("home_lat")), num(rep.get("home_lon"))
    if hlat and hlon:
        layers += [
            dl.Circle(center=[hlat, hlon],
                      radius=num(rep.get("geofence_radius_m"), 500) or 500,
                      color=C_GEO if geo_on else C_MUTED, weight=2,
                      dashArray=None if geo_on else "6 6",
                      fillColor=C_GEO if geo_on else C_MUTED, fillOpacity=0.10),
            dl.CircleMarker(center=[hlat, hlon], radius=5, color=C_GEO, weight=2,
                            fillColor="#0d0d0d", fillOpacity=1,
                            children=dl.Tooltip("Geofence home point")),
        ]
    if pos:
        if acc:
            layers.append(dl.Circle(center=pos, radius=acc, color=C_TRACE, weight=1,
                                    opacity=0.35, fillColor=C_TRACE, fillOpacity=0.12))
        # Stale position (bike offline) gets a greyed marker: this is the last
        # known point, not a live one.
        mk_fill = (C_CRIT if alarm_live else C_TRACE) if online else C_MUTED
        mk_ring = ("#ffffff" if alarm_live else "#12223a") if online else "#5a5954"
        layers.append(dl.CircleMarker(
            center=pos, radius=7, weight=3, color=mk_ring, fillColor=mk_fill,
            fillOpacity=1 if online else 0.6,
            children=dl.Tooltip(f"{pos[0]:.5f}, {pos[1]:.5f} · {ago(pos_ts)}"
                                + ("" if online else " (offline)"))))

    # -- alerts
    rows = []
    for when, a in list(alerts)[:14]:
        label, color, icon = ALERTS.get(int(num(a.get("cause"), 0) or 0),
                                        (str(a.get("alert", "?")), C_MUTED, "•"))
        detail = []
        if a.get("dist_m") is not None:
            detail.append(f"{int(num(a['dist_m'], 0))} m from home point")
        if a.get("lat") is not None:
            detail.append(f"{num(a['lat']):.5f}, {num(a['lon']):.5f}")
        rows.append(html.Div(className="alert", children=[
            html.Div(icon, className="ic"),
            html.Div([
                html.Div(label, className="m", style={"color": color}),
                html.Div(" · ".join(detail), className="d") if detail else None,
            ]),
            html.Div(when.strftime("%H:%M:%S"), className="t",
                     style={"marginLeft": "auto"}),
        ]))
    if not rows:
        rows = [html.Div("No alert today.", className="empty")]

    # -- remote control
    arm_cls = ("on alarm" if alarm_live else "on armed") if armed else ""
    state_line = [
        html.Span("🚨 Motion detected — unlock to clear the alarm."
                  if alarm_live else
                  "The bike alerts on the slightest movement (wheel or IMU)." if armed else
                  "No motion monitoring.", className="big"),
    ]
    if wait_armed:
        txt, cls = wait_badge(armed_since, sm, online)
        state_line.append(html.Span(txt, className=cls))
    elif not online and (armed or alarm):
        # Armed (or alarming at last contact) but silent: detection runs on
        # board and the alert will arrive on reconnection.
        state_line.append(html.Span("📴 offline", className="wait"))

    geo_btn = ("Disable geofence" if geo_on else "Enable geofence") + \
              (" ⏳" if wait_geo else "")
    if hlat and hlon:
        info = [f"Home point {hlat:.6f}, {hlon:.6f} · radius "
                f"{int(num(rep.get('geofence_radius_m'), 500))} m"]
        if pos:
            d = distance_m(hlat, hlon, pos[0], pos[1])
            info.append(f" — bike {d:.0f} m away ({'inside' if d <= num(rep.get('geofence_radius_m'), 500) else 'OUTSIDE'} the zone)")
        if not geo_on:
            # Without this reminder the line reads like a working geofence —
            # "bike 420 m away (OUTSIDE the zone)" — while no alert will ever
            # fire. The dashed circle and the button label were the only hints,
            # and they were far too subtle.
            info = [html.B("⛔ Geofence disabled", className="off"),
                    " — neither exits nor returns will be reported.", html.Br(), *info]
    else:
        info = ["No home point set — the geofence stays inactive."]

    return (chips, tiles, "tiles" if online else "tiles stale", layers, rows,
            f"{len(alerts)} today", arm_cls, "" if armed else "on",
            state_line, geo_btn, info)


@app.callback(Output("panel-live", "style"), Output("panel-trips", "style"),
              Output("panel-stats", "style"), Input("tabs", "value"))
def switch_tab(tab):
    """The three panels stay mounted (a plain CSS toggle): unmounting the hidden
    one would break the callbacks that target its components."""
    hide, show = {"display": "none"}, {}
    return tuple(show if tab == name else hide
                 for name in ("live", "trips", "stats"))


# Two browser-side things in one callback (it fires on load and on every tab
# change):
#
# 1. A window resize. Leaflet and Plotly both listen for it and re-measure,
#    which repairs any component rendered while its panel was hidden.
# 2. PAUSING the tick when the browser tab goes to the background. Under
#    polling, a forgotten tab would otherwise keep querying AWS every 10 s for a
#    screen nobody is looking at — 8600 requests a day, and as many Lambda
#    invocations once deployed. On return, the first refresh arrives on the next
#    tick.
app.clientside_callback(
    """function(tab){
        window.setTimeout(function(){
            window.dispatchEvent(new Event('resize'));
        }, 60);
        if(!window._ebikeVisibility){
            window._ebikeVisibility = true;
            document.addEventListener('visibilitychange', function(){
                window.dash_clientside.set_props('tick', {disabled: document.hidden});
            });
        }
        return window.dash_clientside.no_update;
    }""",
    Output("tabs", "className"),
    Input("tabs", "value"),
)


@app.callback(
    Output("trip-layers", "children"), Output("trip-map", "viewport"),
    Output("trip-map", "invalidateSize"),
    Output("trip-tiles", "children"), Output("trip-meta", "children"),
    Input("panel-trips", "style"), Input("trip-day", "value"),
    Input("trip-reload", "n_clicks"))
def refresh_trips(style, day, _n):
    """Triggered by the panel STYLE, not by tabs.value: a Leaflet map
    initialised inside a display:none container measures itself as 0x0 and stays
    a tiny square in the corner. Depending on the style means this callback only
    fires AFTER switch_tab made the panel visible, so the map has its real size
    when it is invalidated and framed. invalidateSize must change value to have
    any effect, hence the timestamp.
    """
    # Like the charts: only hit DynamoDB when the tab is visible.
    if (style or {}).get("display") == "none":
        raise PreventUpdate
    resize = datetime.now().timestamp()
    try:
        segments, s, alerts = data.load_trips(datetime.fromisoformat(day).date())
    except Exception as e:
        return [], no_update, resize, [], f"Load failed: {e}"

    if not segments:
        return ([], no_update, resize, [tile("No trip", "—")],
                f"No GNSS point on {day}")

    layers, lats, lons = [], [], []
    for i, seg in enumerate(segments):
        pos = [[p[3], p[4]] for p in seg]
        lats += [p[3] for p in seg]
        lons += [p[4] for p in seg]
        layers.append(dl.Polyline(positions=pos, color=C_TRACE, weight=4,
                                  opacity=0.85))
        # Start and end of each stretch: segments are separated by a loss of
        # fix, so it must be visible where the trace stops and resumes.
        t0 = datetime.fromtimestamp(seg[0][0] / 1000)
        t1 = datetime.fromtimestamp(seg[-1][0] / 1000)
        layers += [
            dl.CircleMarker(center=pos[0], radius=6, weight=2, color="#12223a",
                            fillColor=C_GOOD, fillOpacity=1,
                            children=dl.Tooltip(
                                f"Segment {i + 1} — start {t0:%H:%M:%S}")),
            dl.CircleMarker(center=pos[-1], radius=6, weight=2, color="#12223a",
                            fillColor=C_CRIT, fillOpacity=1,
                            children=dl.Tooltip(
                                f"Segment {i + 1} — end {t1:%H:%M:%S}")),
        ]

    for when, a in alerts:
        lat, lon = num(a.get("lat")), num(a.get("lon"))
        if lat is None or lon is None:      # la cause 1 n'a pas encore de position
            continue
        label, color, icon = ALERTS.get(int(num(a.get("cause"), 0) or 0),
                                        ("Alert", C_WARN, "⚠"))
        layers.append(dl.CircleMarker(
            center=[lat, lon], radius=8, weight=3, color=color,
            fillColor="#ffffff", fillOpacity=0.9,
            children=dl.Tooltip(f"{icon} {label} — {when:%H:%M:%S}")))

    viewport = {"bounds": [[min(lats), min(lons)], [max(lats), max(lons)]],
                "transition": "fitBounds",
                "options": {"padding": [30, 30], "maxZoom": 18}}
    tiles = [
        tile("Distance", f"{s['dist_km']:.2f}", " km"),
        tile("Moving", hms(s["moving_s"])),
        tile("Top speed", f"{s['top_kmh']:.1f}", " km/h"),
        tile("Segments", f"{s['segments']}"),
        tile("First fix", f"{s['first']:%H:%M}" if s["first"] else "—"),
        tile("Last fix", f"{s['last']:%H:%M}" if s["last"] else "—"),
        tile("GNSS points", f"{s['points']}"),
        tile("Alerts", f"{len(alerts)}"),
    ]
    meta = (f"{s['points']} points · {s['segments']} segment(s) · "
            f"distance counted above {data.TRIP_MOVE_KMH:.0f} km/h · "
            f"updated {datetime.now():%H:%M:%S}")
    return layers, viewport, resize, tiles, meta


@app.callback(
    Output("g-speed", "figure"), Output("g-accel", "figure"),
    Output("g-imu", "figure"), Output("g-signal", "figure"),
    Output("g-sats", "figure"), Output("g-precision", "figure"),
    Output("hist-meta", "children"),
    Input("panel-stats", "style"), Input("hist-range", "value"),
    Input("hist-reload", "n_clicks"))
def refresh_stats(style, rng, _n):
    # Loaded only when the Stats tab is visible, which avoids hitting DynamoDB
    # on every tick: on tab switch, on range change, or on Reload. As with the
    # map, this depends on the panel STYLE rather than tabs.value, because a
    # Plotly figure drawn in a hidden container gets zero width.
    if (style or {}).get("display") == "none":
        raise PreventUpdate
    now = datetime.now()
    if rng == "today":
        start = int(datetime.combine(now.date(), dtime.min).timestamp() * 1000)
    else:
        start = int((now.timestamp() - {"24h": 24, "7d": 168}[rng] * 3600) * 1000)
    try:
        status_rows, gnss = data.query_history(start)
    except Exception as e:
        empty = charts.fig_speed([], [])
        return (empty,) * 6 + (f"Load failed: {e}",)
    meta = f"{len(status_rows)} status · {len(gnss)} GNSS points · updated {now:%H:%M:%S}"
    return (charts.fig_speed(status_rows, gnss),
            charts.fig_accel(gnss, status_rows),
            charts.fig_imu(status_rows),
            charts.fig_signal(status_rows),
            charts.fig_sats(status_rows),
            charts.fig_precision(gnss),
            meta)


if __name__ == "__main__":
    # Forced first load so the page does not open empty (the tick would take
    # TICK_MS to fill it), and so a credentials problem shows up immediately.
    snap = data.snapshot(force=True)
    print(f"[start] {len(snap['trace'])} points, {len(snap['alerts'])} alerts "
          f"today · shadow reported: {snap['reported']}")
    # app.run(debug=False, host="127.0.0.1", port=8050)
    app.run(debug=False, host="0.0.0.0", port=8050)
