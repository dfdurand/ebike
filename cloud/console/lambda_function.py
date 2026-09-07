#!/usr/bin/env python3
"""eBike console in Lambda: HTTP Basic Auth plus an event-to-WSGI bridge.

This file is the ONLY thing that differs between the local console and the
deployed one. It holds no business logic: it authenticates, translates the
HTTP API event (payload format 2.0) into a WSGI environment, calls the Dash
app of dashboard.py, and translates the response back.

Authentication is Basic Auth because the URL is public (no SigV4 signature is
possible from a browser), so protection has to happen here. The password is
never stored: only a PBKDF2-SHA256 derivative lives in the environment
variables, encrypted at rest by KMS. Successful checks are cached in the warm
container, otherwise the 100000 iterations would be paid again on EVERY request
— page, assets and ticks alike.

Hard limit: a Lambda response cannot exceed 6 MB. That is what compress=True in
dashboard.py is for — without it plotly.min.js (4.85 MB, 6.5 MB once base64
encoded) blows past the cap and the page stays blank.
"""

import base64
import binascii
import hashlib
import hmac
import os
import sys
from io import BytesIO
from urllib.parse import unquote

from dashboard import server as wsgi_app

REALM = "eBike console"
USER = os.environ.get("EBIKE_AUTH_USER", "")
SALT = bytes.fromhex(os.environ.get("EBIKE_AUTH_SALT", "") or "00")
DIGEST = os.environ.get("EBIKE_AUTH_HASH", "")
ITERATIONS = 100_000

# Lambda response: 6 MB in total. Keep a margin for the headers.
MAX_BODY = 6 * 1024 * 1024 - 64 * 1024

_accepted = set()          # credentials already validated (warm container)


def _derive(password: str) -> str:
    return hashlib.pbkdf2_hmac("sha256", password.encode(), SALT, ITERATIONS).hex()


def _authorized(headers: dict) -> bool:
    if not (USER and DIGEST):          # misdeployed: close rather than open
        print("[auth] EBIKE_AUTH_USER/HASH missing — access denied")
        return False
    raw = headers.get("authorization") or headers.get("Authorization") or ""
    if not raw.lower().startswith("basic "):
        return False
    token = raw.split(None, 1)[1].strip()
    if token in _accepted:
        return True
    try:
        user, _, password = base64.b64decode(token).decode("utf-8").partition(":")
    except (binascii.Error, UnicodeDecodeError, ValueError):
        return False
    ok = hmac.compare_digest(user, USER) and hmac.compare_digest(_derive(password),
                                                                 DIGEST)
    if ok:
        _accepted.add(token)
    return ok


def _environ(event: dict) -> dict:
    http = (event.get("requestContext") or {}).get("http") or {}
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    body = event.get("body") or ""
    raw = base64.b64decode(body) if event.get("isBase64Encoded") else body.encode()

    env = {
        "REQUEST_METHOD": http.get("method", "GET"),
        "SCRIPT_NAME": "",
        # rawPath is percent-encoded; WSGI expects the decoded path.
        "PATH_INFO": unquote(event.get("rawPath") or http.get("path") or "/"),
        "QUERY_STRING": event.get("rawQueryString", "") or "",
        "SERVER_NAME": headers.get("host", "lambda"),
        "SERVER_PORT": "443",
        "SERVER_PROTOCOL": http.get("protocol", "HTTP/1.1"),
        "REMOTE_ADDR": http.get("sourceIp", ""),
        "CONTENT_TYPE": headers.get("content-type", ""),
        "CONTENT_LENGTH": str(len(raw)),
        "wsgi.version": (1, 0),
        "wsgi.url_scheme": "https",
        "wsgi.input": BytesIO(raw),
        "wsgi.errors": sys.stderr,
        "wsgi.multithread": False,
        "wsgi.multiprocess": False,
        "wsgi.run_once": False,
    }
    for key, value in headers.items():
        name = "HTTP_" + key.upper().replace("-", "_")
        if name not in ("HTTP_CONTENT_TYPE", "HTTP_CONTENT_LENGTH"):
            env[name] = value
    if event.get("cookies"):
        env["HTTP_COOKIE"] = "; ".join(event["cookies"])
    return env


def _respond(status: int, text: str, **headers) -> dict:
    return {"statusCode": status,
            "headers": {"Content-Type": "text/plain; charset=utf-8", **headers},
            "body": text}


def lambda_handler(event, context):
    if not _authorized(event.get("headers") or {}):
        return _respond(401, "Authentication required.\n",
                        **{"WWW-Authenticate": f'Basic realm="{REALM}", charset="UTF-8"'})

    captured = {}

    def start_response(status, response_headers, exc_info=None):
        captured["status"] = int(status.split(" ", 1)[0])
        captured["headers"] = response_headers

    chunks = wsgi_app(_environ(event), start_response)
    try:
        body = b"".join(chunks)
    finally:
        if hasattr(chunks, "close"):
            chunks.close()

    headers, cookies = {}, []
    for key, value in captured.get("headers", []):
        if key.lower() == "set-cookie":
            cookies.append(value)
        else:
            headers[key] = value

    if len(body) > MAX_BODY:
        # Explicit diagnosis: without it the Lambda returns an opaque error.
        print(f"[wsgi] response too large: {len(body)} bytes for "
              f"{event.get('rawPath')} (is compression enabled?)")
        return _respond(500, "Response too large for a Lambda response.\n")

    out = {"statusCode": captured.get("status", 500), "headers": headers,
           "body": base64.b64encode(body).decode(), "isBase64Encoded": True}
    if cookies:
        out["cookies"] = cookies
    return out
