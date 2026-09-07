#!/usr/bin/env python3
"""Small CLI for the Simbase v2 API (https://developer.simbase.com/).

Optional helper for the LTE-M SIM used by the tracker: check the remaining
credit, the data consumed, and where the SIM last attached. Nothing in the
firmware or the cloud pipeline depends on it.

The API key is read, in order:
    1. the SIMBASE_API_KEY environment variable
    2. cloud/secrets/simbase.json: {"api_key": "your_key"}

If the secrets file also holds "iccid", the info/location/enable/disable
commands use it by default when the argument is omitted.

Usage:
    python3 simbase_cli.py balance                  # remaining credit
    python3 simbase_cli.py sims                     # SIM list (ICCID, state, name)
    python3 simbase_cli.py info <iccid>             # network, session, location
    python3 simbase_cli.py usage [--month YYYY-MM]   # data used (MB) per SIM
    python3 simbase_cli.py location <iccid>         # last network attachments
    python3 simbase_cli.py enable <iccid>           # enable the SIM
    python3 simbase_cli.py disable <iccid>          # disable the SIM

Create the API key in the Simbase dashboard (Settings -> API keys).
"""

import argparse
import json
import os
import sys
from pathlib import Path

import requests

BASE_URL = "https://api.simbase.com/v2"
SECRETS = Path(__file__).resolve().parent / "secrets" / "simbase.json"


def die(msg: str) -> None:
    print(f"Error: {msg}", file=sys.stderr)
    sys.exit(1)


def load_secrets() -> dict:
    return json.loads(SECRETS.read_text()) if SECRETS.exists() else {}


def get_api_key() -> str:
    key = os.environ.get("SIMBASE_API_KEY") or load_secrets().get("api_key")
    if key:
        return key
    die(
        f"API key not found: set SIMBASE_API_KEY or create {SECRETS}\n"
        '  with: {"api_key": "your_key"}'
    )


def resolve_iccid(args) -> str:
    iccid = args.iccid or load_secrets().get("iccid")
    if not iccid:
        die(f'Missing ICCID: pass it as an argument or add "iccid" to {SECRETS}')
    return iccid


def api(method: str, endpoint: str, params=None, body=None):
    key = get_api_key()
    resp = requests.request(
        method,
        f"{BASE_URL}{endpoint}",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
        params=params,
        json=body,
        timeout=30,
    )
    if resp.status_code == 401:
        die("invalid API key (401)")
    if resp.status_code == 429:
        die("rate limit reached (429), try again later")
    if not resp.ok:
        die(f"{method} {endpoint} -> HTTP {resp.status_code}: {resp.text}")
    return resp.json() if resp.text else {}


def paginate(endpoint: str, params=None):
    """Iterate over every page of a cursor-paginated endpoint."""
    params = dict(params or {})
    params.setdefault("limit", 100)
    while True:
        resp = api("GET", endpoint, params=params)
        if isinstance(resp, list):
            yield from resp
            return
        yield from (
            resp.get("data") or resp.get("simcards") or resp.get("items") or []
        )
        if not resp.get("has_more"):
            return
        cursor = resp.get("cursor") or resp.get("next_cursor")
        if not cursor:
            return
        params["cursor"] = cursor


def cmd_balance(_args):
    bal = api("GET", "/account/balance")
    amount = bal.get("balance", bal.get("amount"))
    currency = bal.get("currency", "")
    if amount is not None:
        print(f"Remaining credit: {amount} {currency}".strip())
    else:
        print(json.dumps(bal, indent=2))


def cmd_sims(_args):
    for sim in paginate("/simcards"):
        iccid = sim.get("iccid", "?")
        state = sim.get("state", sim.get("status", "?"))
        name = sim.get("name") or ""
        print(f"{iccid}  {state:<10} {name}")


def cmd_info(args):
    sim = api("GET", f"/simcards/{resolve_iccid(args)}")
    if args.json:
        print(json.dumps(sim, indent=2, ensure_ascii=False))
        return
    loc = sim.get("location") or {}
    usage = sim.get("current_month_usage") or {}
    costs = sim.get("current_month_costs") or {}
    data_mb = (usage.get("data") or 0) / 1e6
    lines = [
        ("ICCID", sim.get("iccid")),
        ("Name", sim.get("name") or "-"),
        ("State", sim.get("state")),
        ("Plan", f"{sim.get('plan_id')} ({sim.get('coverage')})"),
        ("Hardware", f"{sim.get('hardware') or '?'} / IMEI {sim.get('imei') or '?'}"),
        ("Network", f"{loc.get('carrier')} ({loc.get('country')}, {loc.get('radio')})"),
        (
            "Cell",
            f"MCC {loc.get('mcc')} MNC {loc.get('mnc')} "
            f"LAC {loc.get('lac')} cell {loc.get('cell_id')}",
        ),
        ("Last seen", loc.get("last_update")),
        ("Session", sim.get("session_status")),
        ("Public IP", sim.get("public_ip") or "-"),
        (
            "Data (month)",
            f"{data_mb:.2f} MB over {usage.get('data_sessions', 0)} sessions",
        ),
        (
            "Cost (month)",
            f"{costs.get('total', '?')} "
            f"(data {costs.get('data', '?')} + line rental {costs.get('line_rental', '?')})",
        ),
    ]
    for label, value in lines:
        print(f"{label:<12}: {value}")


def cmd_usage(args):
    params = {"month": args.month} if args.month else None
    total_mb = 0.0
    total_cost = 0.0
    for rec in paginate("/usage/simcards", params=params):
        iccid = rec.get("iccid", "?")
        mb = (rec.get("usage", {}).get("data") or 0) / 1e6  # data comes in bytes
        cost = float(rec.get("costs", {}).get("total") or 0)
        total_mb += mb
        total_cost += cost
        print(f"{iccid}  {mb:8.2f} MB  cost {cost:.4f}")
    print(f"\nTotal: {total_mb:.2f} MB, cost {total_cost:.4f}")


def cmd_location(args):
    resp = api(
        "GET", f"/simcards/{resolve_iccid(args)}/location-updates", params={"limit": 10}
    )
    updates = resp.get("data") or resp.get("items") or resp
    print(json.dumps(updates, indent=2, ensure_ascii=False))


def _set_state(iccid: str, state: str):
    resp = api("POST", f"/simcards/{iccid}/state", body={"state": state})
    print(f"SIM {iccid} -> {state}")
    if resp:
        print(json.dumps(resp, indent=2))


def cmd_enable(args):
    _set_state(resolve_iccid(args), "enabled")


def cmd_disable(args):
    _set_state(resolve_iccid(args), "disabled")


def main():
    parser = argparse.ArgumentParser(description="Simbase CLI (API v2)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("balance", help="remaining account credit")
    sub.add_parser("sims", help="list the SIMs")

    iccid_help = "SIM ICCID (defaults to the one in secrets/simbase.json)"

    p = sub.add_parser("info", help="SIM details (network, location)")
    p.add_argument("iccid", nargs="?", help=iccid_help)
    p.add_argument("--json", action="store_true", help="print the raw response")

    p = sub.add_parser("usage", help="data used (MB) per SIM")
    p.add_argument("--month", help="month as YYYY-MM (default: current month)")

    p = sub.add_parser("location", help="last network attachments of a SIM")
    p.add_argument("iccid", nargs="?", help=iccid_help)

    p = sub.add_parser("enable", help="enable a SIM")
    p.add_argument("iccid", nargs="?", help=iccid_help)

    p = sub.add_parser("disable", help="disable a SIM")
    p.add_argument("iccid", nargs="?", help=iccid_help)

    args = parser.parse_args()
    {
        "balance": cmd_balance,
        "sims": cmd_sims,
        "info": cmd_info,
        "usage": cmd_usage,
        "location": cmd_location,
        "enable": cmd_enable,
        "disable": cmd_disable,
    }[args.command](args)


if __name__ == "__main__":
    main()
