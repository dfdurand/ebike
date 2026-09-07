#!/usr/bin/env python3
"""Provision the AWS side of the eBike pipeline. Idempotent, safe to re-run.

Creates (or finds) everything the device needs to publish into:
  - DynamoDB table `ebike-telemetry` (PK device_id, SK ts), provisioned 5/5 so
    it stays inside the permanent free tier
  - SNS topic `ebike-alerts` plus an e-mail subscription (AWS sends a
    confirmation link you must click). The e-mail body itself is formatted by
    the Lambda of deploy_alert_notifier.py, which publishes to this topic; here
    we only archive.
  - IAM role `ebike-iot-rule-role`, assumed by iot.amazonaws.com
  - IoT rules: bike/+/telemetry, bike/+/status and bike/+/alert -> DynamoDB

Environment:
  EBIKE_REGION       AWS region (default eu-west-3)
  EBIKE_TABLE        DynamoDB table name (default ebike-telemetry)
  EBIKE_ALERT_TOPIC  SNS topic name (default ebike-alerts)

Usage:
  .venv/bin/python provision.py --email you@example.com
"""

import argparse
import json
import os
import sys
import time

import boto3
from botocore.exceptions import ClientError

REGION = os.environ.get("EBIKE_REGION", "eu-west-3")
TABLE = os.environ.get("EBIKE_TABLE", "ebike-telemetry")
TOPIC = os.environ.get("EBIKE_ALERT_TOPIC", "ebike-alerts")
ROLE = "ebike-iot-rule-role"
SQL_VERSION = "2016-03-23"

# The firmware payload carries neither an identifier nor a timestamp: the rule
# injects both.
SQL = "SELECT *, topic(2) AS device_id, timestamp() AS ts FROM '{}'"

session = boto3.session.Session(region_name=REGION)
ddb = session.client("dynamodb")
sns = session.client("sns")
iam = session.client("iam")
iot = session.client("iot")


def ensure_table() -> str:
    try:
        desc = ddb.describe_table(TableName=TABLE)["Table"]
        print(f"[=] DynamoDB table {TABLE} already exists ({desc['TableStatus']})")
    except ClientError as e:
        if e.response["Error"]["Code"] != "ResourceNotFoundException":
            raise
        ddb.create_table(
            TableName=TABLE,
            AttributeDefinitions=[
                {"AttributeName": "device_id", "AttributeType": "S"},
                {"AttributeName": "ts", "AttributeType": "N"},
            ],
            KeySchema=[
                {"AttributeName": "device_id", "KeyType": "HASH"},
                {"AttributeName": "ts", "KeyType": "RANGE"},
            ],
            # Provisioned 5/5 sits in the permanent free tier (25 RCU/WCU),
            # unlike on-demand which is billed per request.
            ProvisionedThroughput={"ReadCapacityUnits": 5, "WriteCapacityUnits": 5},
        )
        print(f"[+] DynamoDB table {TABLE} created, waiting for ACTIVE...")
        ddb.get_waiter("table_exists").wait(TableName=TABLE)
        desc = ddb.describe_table(TableName=TABLE)["Table"]
    return desc["TableArn"]


def ensure_sns(email: str) -> str:
    topic_arn = sns.create_topic(Name=TOPIC)["TopicArn"]  # idempotent
    print(f"[=] SNS topic {topic_arn}")
    subs = sns.list_subscriptions_by_topic(TopicArn=topic_arn)["Subscriptions"]
    match = [s for s in subs if s["Endpoint"] == email]
    if match:
        print(f"[=] E-mail subscription {email} already present ({match[0]['SubscriptionArn']})")
    else:
        sns.subscribe(TopicArn=topic_arn, Protocol="email", Endpoint=email)
        print(f"[+] E-mail subscription {email} created — CLICK the confirmation link")
    return topic_arn


def ensure_role(table_arn: str) -> str:
    trust = {
        "Version": "2012-10-17",
        "Statement": [{
            "Effect": "Allow",
            "Principal": {"Service": "iot.amazonaws.com"},
            "Action": "sts:AssumeRole",
        }],
    }
    # No sns:Publish here: SNS publication is done by the notifier Lambda under
    # its own role. The IoT rules only write to DynamoDB.
    policy = {
        "Version": "2012-10-17",
        "Statement": [
            {"Effect": "Allow", "Action": "dynamodb:PutItem", "Resource": table_arn},
        ],
    }
    try:
        arn = iam.get_role(RoleName=ROLE)["Role"]["Arn"]
        print(f"[=] IAM role {ROLE} already exists")
    except ClientError as e:
        if e.response["Error"]["Code"] != "NoSuchEntity":
            raise
        arn = iam.create_role(
            RoleName=ROLE,
            AssumeRolePolicyDocument=json.dumps(trust),
            Description="Lets the eBike IoT rules write to DynamoDB",
        )["Role"]["Arn"]
        print(f"[+] IAM role {ROLE} created")
    # Re-applied on every run: the policy follows the table ARN.
    iam.put_role_policy(
        RoleName=ROLE,
        PolicyName="ebike-iot-rule-policy",
        PolicyDocument=json.dumps(policy),
    )
    return arn


def ddb_action(role_arn: str) -> dict:
    return {"dynamoDBv2": {"roleArn": role_arn, "putItem": {"tableName": TABLE}}}


def ensure_rule(name: str, topic_filter: str, actions: list) -> None:
    payload = {
        "sql": SQL.format(topic_filter),
        "awsIotSqlVersion": SQL_VERSION,
        "actions": actions,
        "ruleDisabled": False,
    }
    exists = name in {r["ruleName"] for r in iot.list_topic_rules()["rules"]}
    # Right after the role is created, IoT may refuse to assume it while IAM
    # propagates, so retry a few times.
    for attempt in range(6):
        try:
            if exists:
                iot.replace_topic_rule(ruleName=name, topicRulePayload=payload)
                print(f"[=] IoT rule {name} updated ({topic_filter})")
            else:
                iot.create_topic_rule(ruleName=name, topicRulePayload=payload)
                print(f"[+] IoT rule {name} created ({topic_filter})")
            return
        except ClientError as e:
            if e.response["Error"]["Code"] == "InvalidRequestException" and attempt < 5:
                print("    ... IAM role not propagated yet, retrying in 5 s")
                time.sleep(5)
                continue
            raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--email", required=True,
                        help="recipient of the SNS alert e-mails")
    args = parser.parse_args()

    table_arn = ensure_table()
    topic_arn = ensure_sns(args.email)
    role_arn = ensure_role(table_arn)

    ensure_rule("ebike_telemetry_ddb", "bike/+/telemetry", [ddb_action(role_arn)])
    ensure_rule("ebike_status_ddb", "bike/+/status", [ddb_action(role_arn)])
    # Alerts go to DynamoDB for archival only. The formatted SNS e-mail is the
    # job of the notifier Lambda, wired to its own rule by
    # deploy_alert_notifier.py.
    ensure_rule("ebike_alert_ddb", "bike/+/alert", [ddb_action(role_arn)])

    print("\nSummary:")
    print(f"  table  {table_arn}")
    print(f"  topic  {topic_arn}")
    print(f"  role   {role_arn}")
    print("  rules  ebike_telemetry_ddb, ebike_status_ddb, ebike_alert_ddb")
    print("  email  formatted by the notifier Lambda: deploy_alert_notifier.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
