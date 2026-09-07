#!/usr/bin/env python3
"""Deploy the eBike console on AWS: Lambda plus an HTTP API. Idempotent.

Architecture:

    browser --HTTPS--> HTTP API --> Lambda ebike-console
                                     | (Basic Auth in the handler)
                                     | IAM role ebike-console-lambda-role
                                     +--> DynamoDB ebike-telemetry (Query)
                                     +--> IoT      Get/UpdateThingShadow
                                     +--> IoT      Publish bike/<id>/cmd

An HTTP API is used rather than a Lambda Function URL. Some accounts refuse
anonymous invocation of a Function URL even with the documented resource policy
(Principal "*" plus lambda:FunctionUrlAuthType NONE), returning 403; the same
URL with AWS_IAM and a signed request reaches the function, so it is the public
path that is blocked and no SDK call lifts that guard. The HTTP API accepts
public traffic, uses the SAME event format (payload 2.0, so the handler is
unchanged) and forwards the Authorization header as long as no authorizer is
attached. Cost: about 1 USD per million requests instead of zero.

NO device certificate lives on the server: the console reads DynamoDB and the
Device Shadow with an IAM role. See the header of ebike_data.py.

The package embeds Dash, Plotly and dash-leaflet (~20 MB zipped, 88 MB
unpacked; Lambda caps are 50 MB on upload and 250 MB unpacked). boto3 is not
included: the python3.12 runtime provides it.

Basic Auth credentials live in cloud/secrets/console.json (chmod 600, not
versioned). The file is CREATED with a random password if it does not exist,
and only a PBKDF2 derivative is sent to AWS.

Environment:
  EBIKE_REGION  AWS region (default eu-west-3)
  EBIKE_DEVICE  thing name, must match CONFIG_AWS_IOT_CLIENT_ID_STATIC
  EBIKE_TABLE   DynamoDB table name
  EBIKE_TZ      time zone of the console (default Europe/Paris)

Usage:
  .venv/bin/python deploy_console.py            # build (cached) and deploy
  .venv/bin/python deploy_console.py --fresh    # rebuild the dependencies
  .venv/bin/python deploy_console.py --url      # print the public URL only
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import time
import zipfile
from pathlib import Path

import boto3
from botocore.exceptions import ClientError

REGION = os.environ.get("EBIKE_REGION", "eu-west-3")
DEVICE = os.environ.get("EBIKE_DEVICE", "ebike-001")
TABLE = os.environ.get("EBIKE_TABLE", "ebike-telemetry")
TZ = os.environ.get("EBIKE_TZ", "Europe/Paris")
FUNC = "ebike-console"
ROLE = "ebike-console-lambda-role"
API = "ebike-console-api"

HERE = Path(__file__).resolve().parent
SECRETS = HERE / "secrets" / "console.json"
BUILD = HERE / "build" / "console"
APP_FILES = ("dashboard.py", "charts.py", "ebike_data.py")
HANDLER = HERE / "console" / "lambda_function.py"
REQUIREMENTS = ("dash[compress]", "dash-leaflet", "plotly")

# Dead weight in the package. dash_renderer.dev.js (8 MB) is only served in
# debug mode, labextension and widgetbundle.js are the Jupyter plugins of
# Plotly, and setuptools has no business inside a Lambda.
PRUNE = ("setuptools", "pkg_resources", "share", "plotly/labextension",
         "plotly/package_data/widgetbundle.js",
         "dash/dash-renderer/build/dash_renderer.dev.js")

ITERATIONS = 100_000        # must stay aligned with console/lambda_function.py

session = boto3.session.Session(region_name=REGION)
lam = session.client("lambda")
iam = session.client("iam")
sts = session.client("sts")
apigw = session.client("apigatewayv2")


# ------------------------------------------------------------------- credentials
def load_auth() -> tuple:
    """(user, salt_hex, digest_hex). The secrets file is created if needed."""
    if SECRETS.exists():
        creds = json.loads(SECRETS.read_text())
        created = False
    else:
        import secrets as pysecrets
        creds = {"user": "ebike", "password": pysecrets.token_urlsafe(12)}
        SECRETS.parent.mkdir(parents=True, exist_ok=True)
        SECRETS.write_text(json.dumps(creds, indent=2) + "\n")
        SECRETS.chmod(stat.S_IRUSR | stat.S_IWUSR)
        created = True

    user, password = creds["user"], creds["password"]
    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, ITERATIONS).hex()
    if created:
        print(f"[+] {SECRETS.relative_to(HERE.parent)} created (chmod 600)\n"
              f"    user     : {user}\n"
              f"    password : {password}")
    else:
        print(f"[=] Credentials read from {SECRETS.relative_to(HERE.parent)} "
              f"(user '{user}')")
    return user, salt.hex(), digest


# ------------------------------------------------------------------------- build
def build_zip(fresh: bool) -> bytes:
    if fresh and BUILD.exists():
        shutil.rmtree(BUILD)
    if not (BUILD / "dash").exists():
        BUILD.mkdir(parents=True, exist_ok=True)
        print("[.] Installing dependencies (about a minute)...")
        # sys.executable is the venv python: same version (3.12) and
        # architecture as the Lambda runtime, so the binary wheels (brotli,
        # markupsafe) are the right ones.
        subprocess.run([sys.executable, "-m", "pip", "install", "-q",
                        "--target", str(BUILD), *REQUIREMENTS], check=True)
        for rel in PRUNE:
            target = BUILD / rel
            if target.is_dir():
                shutil.rmtree(target)
            elif target.exists():
                target.unlink()
        for cache in BUILD.rglob("__pycache__"):
            shutil.rmtree(cache, ignore_errors=True)
        print(f"[+] Dependencies ready ({du_mb(BUILD):.0f} MB)")
    else:
        print(f"[=] Dependencies already built ({du_mb(BUILD):.0f} MB) "
              f"— use --fresh to rebuild")

    for name in APP_FILES:
        shutil.copy(HERE / name, BUILD / name)
    shutil.copy(HANDLER, BUILD / "lambda_function.py")

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(BUILD.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                z.write(path, path.relative_to(BUILD).as_posix())
    code = buf.getvalue()
    print(f"[+] Package: {len(code) / 1e6:.1f} MB zipped (upload cap: 50 MB)")
    if len(code) > 50 * 1024 * 1024:
        sys.exit("Package too large for a direct upload — it would need S3.")
    return code


def du_mb(path: Path) -> float:
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file()) / 1e6


# --------------------------------------------------------------------------- IAM
def ensure_role(account: str) -> str:
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
    # Strictly what the console needs: read the device history, read and write
    # ITS shadow, publish on ITS command topic.
    iam.put_role_policy(
        RoleName=ROLE, PolicyName="ebike-console-access",
        PolicyDocument=json.dumps({"Version": "2012-10-17", "Statement": [
            {"Effect": "Allow", "Action": "dynamodb:Query",
             "Resource": f"arn:aws:dynamodb:{REGION}:{account}:table/{TABLE}"},
            {"Effect": "Allow",
             "Action": ["iot:GetThingShadow", "iot:UpdateThingShadow"],
             "Resource": f"arn:aws:iot:{REGION}:{account}:thing/{DEVICE}"},
            {"Effect": "Allow", "Action": "iot:Publish",
             "Resource": f"arn:aws:iot:{REGION}:{account}:topic/bike/{DEVICE}/cmd"},
        ]}))
    return arn


# ------------------------------------------------------------------------ lambda
def ensure_lambda(role_arn: str, code: bytes, env: dict) -> str:
    # 1024 MB: the cold start imports Dash and Plotly. More memory means more
    # CPU, so a much shorter start at nearly identical cost, since billing is in
    # GB-seconds.
    common = dict(Timeout=30, MemorySize=1024, Environment={"Variables": env})
    for attempt in range(6):
        try:
            lam.create_function(FunctionName=FUNC, Runtime="python3.12",
                                Role=role_arn, Handler="lambda_function.lambda_handler",
                                Code={"ZipFile": code}, **common)
            print(f"[+] Lambda {FUNC} created")
            break
        except ClientError as e:
            name = e.response["Error"]["Code"]
            if name == "ResourceConflictException":
                lam.update_function_code(FunctionName=FUNC, ZipFile=code)
                lam.get_waiter("function_updated_v2").wait(FunctionName=FUNC)
                lam.update_function_configuration(FunctionName=FUNC, **common)
                print(f"[=] Lambda {FUNC} updated (code and configuration)")
                break
            if name == "InvalidParameterValueException" and attempt < 5:
                print("    ... IAM role not propagated yet, retrying in 5 s")
                time.sleep(5)
                continue
            raise
    lam.get_waiter("function_updated_v2").wait(FunctionName=FUNC)
    return lam.get_function(FunctionName=FUNC)["Configuration"]["FunctionArn"]


def api_endpoint() -> str:
    for api in apigw.get_apis()["Items"]:
        if api["Name"] == API:
            return api["ApiEndpoint"] + "/"
    sys.exit(f"HTTP API {API} not found — run the deployment first.")


def ensure_api(account: str, func_arn: str) -> str:
    """HTTP API in quick-create mode: a $default route sending everything to the
    Lambda, and an auto-deployed $default stage, so there is no stage prefix in
    the URL and the Dash paths work as they are.

    No authorizer is attached, deliberately: otherwise the HTTP API would
    interpret the Basic Auth Authorization header instead of forwarding it."""
    api = next((a for a in apigw.get_apis()["Items"] if a["Name"] == API), None)
    if api is None:
        api = apigw.create_api(Name=API, ProtocolType="HTTP", Target=func_arn)
        print(f"[+] HTTP API {API} created")
    else:
        print(f"[=] HTTP API {API} exists")
    try:
        lam.add_permission(
            FunctionName=FUNC, StatementId="apigw-invoke",
            Action="lambda:InvokeFunction", Principal="apigateway.amazonaws.com",
            SourceArn=f"arn:aws:execute-api:{REGION}:{account}:{api['ApiId']}/*/*")
        print("    invoke permission granted to the API")
    except ClientError as e:
        if e.response["Error"]["Code"] != "ResourceConflictException":
            raise
    return api["ApiEndpoint"] + "/"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fresh", action="store_true",
                    help="rebuild the package dependencies")
    ap.add_argument("--url", action="store_true",
                    help="print the public URL and exit")
    args = ap.parse_args()

    if args.url:
        print(api_endpoint())
        return 0

    account = sts.get_caller_identity()["Account"]
    user, salt, digest = load_auth()
    code = build_zip(args.fresh)
    role_arn = ensure_role(account)
    func_arn = ensure_lambda(role_arn, code, {
        "EBIKE_REGION": REGION, "EBIKE_DEVICE": DEVICE, "EBIKE_TABLE": TABLE,
        "EBIKE_AUTH_USER": user, "EBIKE_AUTH_SALT": salt, "EBIKE_AUTH_HASH": digest,
        # Without TZ the Lambda lives in UTC, so "today" and every displayed
        # time would be offset from the bike's local time.
        "TZ": TZ,
    })
    url = ensure_api(account, func_arn)
    print(f"\nDone — console deployed: {url}\n"
          f"    user '{user}', password in "
          f"{SECRETS.relative_to(HERE.parent)}.\n"
          f"    The first load is slow (~5 s): cold start plus assets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
