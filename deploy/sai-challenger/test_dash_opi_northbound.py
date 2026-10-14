# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
"""SAI-Challenger-style northbound checks for the dash-opi lab stack.

Run via scripts/run_sai_challenger.sh (pytest) or:
  pytest deploy/sai-challenger/test_dash_opi_northbound.py -v
"""

from __future__ import annotations

import json
import os
import socket
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PAYLOADS = ROOT / "deploy" / "payloads"
GNMI = os.environ.get("GNMI_ADDR", "127.0.0.1:8080")
REDIS_HOST = os.environ.get("REDIS_HOST", "127.0.0.1")
REDIS_PORT = int(os.environ.get("REDIS_PORT", "6379"))
OPI = os.environ.get("OPI_SERVER_ADDRESS", "127.0.0.1:50051")


def _wait_tcp(addr: str, timeout: float = 30.0) -> None:
    host, port_s = addr.rsplit(":", 1)
    port = int(port_s)
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0):
                return
        except OSError as exc:
            last = exc
            time.sleep(0.2)
    raise AssertionError(f"{addr} not reachable: {last}")


@pytest.fixture(scope="session")
def redis_client():
    redis = pytest.importorskip("redis")
    _wait_tcp(f"{REDIS_HOST}:{REDIS_PORT}")
    c = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=0, decode_responses=True)
    c.ping()
    return c


def test_gnmi_port_open():
    _wait_tcp(GNMI)


def test_payload_files_exist():
    for name in (
        "dash_vnet.json",
        "dash_eni.json",
        "dash_eni_b.json",
        "dash_route.json",
        "dash_acl_rule.json",
        "dash_acl_rule_iperf.json",
    ):
        p = PAYLOADS / name
        assert p.is_file(), p
        json.loads(p.read_text())


def test_appl_db_has_dash_tables_after_push(redis_client):
    """Expect run_sai_challenger.sh to have issued gNMI Sets before pytest."""
    if os.environ.get("SAI_CHALLENGER_SKIP_APPL") == "1":
        pytest.skip("gNMI push not run yet")
    keys = list(redis_client.scan_iter("DASH_*"))
    if not keys:
        pytest.skip("APPL_DB empty — run scripts/run_sai_challenger.sh")
    joined = " ".join(keys)
    assert "DASH_VNET_TABLE" in joined
    assert "DASH_ENI_TABLE" in joined
    assert "DASH_ROUTE_TABLE" in joined
    assert "DASH_ACL" in joined
