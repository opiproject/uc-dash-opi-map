#!/usr/bin/env python3
"""Start the OPI mock server and run tests/test_sai_create_eni against it.

SNAP (:50052) is not required. The adapter logs a SNAP wait miss and still
runs the CloudInfra ENI path.
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / "tests" / "opi_server" / "server.py"


def wait_port(host: str, port: int, timeout: float = 15.0) -> None:
    deadline = time.time() + timeout
    last: Optional[OSError] = None
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as exc:
            last = exc
            time.sleep(0.15)
    raise SystemExit(f"OPI mock server did not listen on {host}:{port}: {last}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--binary",
        default=str(ROOT / "build" / "test_sai_create_eni"),
        help="Path to the test_sai_create_eni executable",
    )
    parser.add_argument("--port", type=int, default=50051)
    args = parser.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        # Visual Studio / multi-config generators
        for candidate in (ROOT / "build").rglob("test_sai_create_eni*"):
            if candidate.is_file() and candidate.suffix.lower() in ("", ".exe"):
                binary = candidate
                break
    if not binary.exists():
        print(
            "test_sai_create_eni not found. Build first:\n"
            "  cmake -B build -S .\n"
            "  cmake --build build --target test_sai_create_eni",
            file=sys.stderr,
        )
        return 1

    listen_host = "127.0.0.1"
    target = f"{listen_host}:{args.port}"
    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"
    server = subprocess.Popen(
        [sys.executable, str(SERVER), "--port", str(args.port)],
        cwd=str(ROOT),
        env=env,
    )
    try:
        wait_port(listen_host, args.port)
        print(f"running {binary} {target}", flush=True)
        test_env = os.environ.copy()
        test_env["SAI_OPI_EXPECT_MOCK_STATS"] = "1"
        result = subprocess.run(
            [str(binary), target], cwd=str(ROOT), env=test_env
        )
        return result.returncode
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    raise SystemExit(main())
