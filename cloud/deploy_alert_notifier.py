#!/usr/bin/env python3
"""Deploy the alert notifier: a Python Lambda plus its IoT rule. Idempotent.

Turns bike/+/alert messages into readable SNS e-mails (a subject per cause and
a verbose body), instead of the raw JSON a plain SNS rule action would send.

  bike/+/alert -+- rule ebike_alert_ddb    -> DynamoDB (archive, provision.py)
                +- rule ebike_alert_notify -> Lambda   -> SNS (formatted mail)

Prerequisite: run provision.py first. It creates the SNS topic and the e-mail
subscription this Lambda publishes to.

Environment:
  EBIKE_REGION       AWS region (default eu-west-3)
  EBIKE_ALERT_TOPIC  SNS topic name (default ebike-alerts)
  EBIKE_TZ           time zone of the e-mail body (default Europe/Paris)

Usage:
  .venv/bin/python deploy_alert_notifier.py
"""

import argparse
import io
import json
import os
import sys
import time
import zipfile
from pathlib import Path

import boto3
from botocore.exceptions import ClientError

REGION = os.environ.get("EBIKE_REGION", "eu-west-3")
TOPIC = os.environ.get("EBIKE_ALERT_TOPIC", "ebike-alerts")
TZ = os.environ.get("EBIKE_TZ", "Europe/Paris")
FUNC = "ebike-alert-notifier"
ROLE = "ebike-alert-lambda-role"
RULE = "ebike_alert_notify"
SRC = Path(__file__).resolve().parent / "alert_notifier" / "lambda_function.py"

session = boto3.session.Session(region_name=REGION)
lam = session.client("lambda")
iam = session.client("iam")
iot = session.client("iot")
sns = session.client("sns")
sts = session.client("sts")


def ensure_topic() -> str:
    """The topic is created by provision.py; create_topic is idempotent."""
    return sns.create_topic(Name=TOPIC)["TopicArn"]


def ensure_role(topic_arn: str) -> str:
    trust = {"Version": "2012-10-17", "Statement": [{
        "Effect": "Allow", "Principal": {"Service": "lambda.amazonaws.com"},
        "Action": "sts:AssumeRole"}]}
    try:
        arn = iam.get_role(RoleName=ROLE)["Role"]["Arn"]
        print(f"[=] Role {ROLE} exists")
    except ClientError as e:
        if e.response["Error"]["Code"] != "NoSuchEntity":
            raise
        arn = iam.create_role(RoleName=ROLE,
                              AssumeRolePolicyDocument=json.dumps(trust))["Role"]["Arn"]
        print(f"[+] Role {ROLE} created")
    iam.attach_role_policy(
        RoleName=ROLE,
        PolicyArn="arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole")
    # Publication restricted to the alert topic alone.
    iam.put_role_policy(
        RoleName=ROLE, PolicyName="ebike-alert-sns-publish",
        PolicyDocument=json.dumps({"Version": "2012-10-17", "Statement": [
            {"Effect": "Allow", "Action": "sns:Publish", "Resource": topic_arn}]}))
    return arn


def zip_code() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("lambda_function.py", SRC.read_text())
    return buf.getvalue()


def ensure_lambda(role_arn: str, env: dict) -> str:
    code = zip_code()
    for attempt in range(6):
        try:
            arn = lam.create_function(
                FunctionName=FUNC, Runtime="python3.12", Role=role_arn,
                Handler="lambda_function.lambda_handler", Code={"ZipFile": code},
                Timeout=15, MemorySize=128, Environment={"Variables": env},
            )["FunctionArn"]
            print(f"[+] Lambda {FUNC} created")
            return arn
        except ClientError as e:
            code_name = e.response["Error"]["Code"]
            if code_name == "ResourceConflictException":
                lam.update_function_code(FunctionName=FUNC, ZipFile=code)
                lam.get_waiter("function_updated_v2").wait(FunctionName=FUNC)
                lam.update_function_configuration(
                    FunctionName=FUNC, Environment={"Variables": env})
                arn = lam.get_function(FunctionName=FUNC)["Configuration"]["FunctionArn"]
                print(f"[=] Lambda {FUNC} updated (code and environment)")
                return arn
            if code_name == "InvalidParameterValueException" and attempt < 5:
                print("    ... IAM role not propagated yet, retrying in 5 s")
                time.sleep(5)
                continue
            raise


def ensure_rule(func_arn: str) -> None:
    sql = "SELECT *, topic(2) AS device_id, timestamp() AS ts FROM 'bike/+/alert'"
    payload = {"sql": sql, "awsIotSqlVersion": "2016-03-23",
               "actions": [{"lambda": {"functionArn": func_arn}}],
               "ruleDisabled": False}
    exists = RULE in {r["ruleName"] for r in iot.list_topic_rules()["rules"]}
    if exists:
        iot.replace_topic_rule(ruleName=RULE, topicRulePayload=payload)
        print(f"[=] Rule {RULE} updated")
    else:
        iot.create_topic_rule(ruleName=RULE, topicRulePayload=payload)
        print(f"[+] Rule {RULE} created")
    account = sts.get_caller_identity()["Account"]
    rule_arn = f"arn:aws:iot:{REGION}:{account}:rule/{RULE}"
    try:
        lam.add_permission(FunctionName=FUNC, StatementId=f"iot-{RULE}",
                           Action="lambda:InvokeFunction",
                           Principal="iot.amazonaws.com", SourceArn=rule_arn)
        print(f"    invoke permission granted to {RULE}")
    except ClientError as e:
        if e.response["Error"]["Code"] != "ResourceConflictException":
            raise


def main() -> int:
    argparse.ArgumentParser(description=__doc__).parse_args()
    topic_arn = ensure_topic()
    role_arn = ensure_role(topic_arn)
    func_arn = ensure_lambda(role_arn, {
        "ALERT_TOPIC_ARN": topic_arn, "TZ": TZ})
    ensure_rule(func_arn)
    print("\nDone. Publish a test alert on bike/<device>/alert (cause 1, 2 or 3) "
          "and check the e-mail you receive.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
