#!/usr/bin/env python3
"""Print the latest items of the telemetry table.

Environment:
  EBIKE_REGION  AWS region (default eu-west-3)
  EBIKE_DEVICE  device id (default ebike-001)
  EBIKE_TABLE   table name (default ebike-telemetry)

Usage:
  .venv/bin/python show_db.py            # last 20 items
  .venv/bin/python show_db.py -n 50      # last 50
  .venv/bin/python show_db.py --today    # everything since midnight
"""

import argparse
import json
import os
from datetime import datetime, time as dtime

import boto3
from boto3.dynamodb.conditions import Key

REGION = os.environ.get("EBIKE_REGION", "eu-west-3")
DEVICE = os.environ.get("EBIKE_DEVICE", "ebike-001")
TABLE = os.environ.get("EBIKE_TABLE", "ebike-telemetry")


def kind(item):
    if "alert" in item:
        return "ALERT"
    if "pts" in item:
        return f"telemetry ({len(item['pts'])} pts)"
    return "status"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("-n", type=int, default=20, help="number of items (default 20)")
    p.add_argument("--today", action="store_true", help="everything since midnight")
    args = p.parse_args()

    table = boto3.session.Session(region_name=REGION).resource("dynamodb").Table(TABLE)
    cond = Key("device_id").eq(DEVICE)
    if args.today:
        midnight = int(datetime.combine(datetime.now().date(), dtime.min).timestamp() * 1000)
        cond &= Key("ts").gte(midnight)

    resp = table.query(KeyConditionExpression=cond,
                       ScanIndexForward=False,          # newest first
                       Limit=10000 if args.today else args.n)
    items = resp["Items"]
    print(f"{len(items)} item(s), newest first\n")
    for it in items:
        t = datetime.fromtimestamp(int(it["ts"]) / 1000).strftime("%Y-%m-%d %H:%M:%S")
        body = {k: v for k, v in it.items() if k not in ("device_id", "ts")}
        print(f"{t}  [{kind(it):18}] {json.dumps(body, default=str, ensure_ascii=False)[:260]}")


if __name__ == "__main__":
    main()
