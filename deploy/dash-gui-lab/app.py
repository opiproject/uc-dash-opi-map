# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
"""
Demo GUI: browser → Flask → sonic-gnmi gNMI Set/Delete (JSON_IETF payloads).
Not grpcui — buttons are DASH objects, not protobuf forms.
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path

import grpc
import redis
from flask import Flask, jsonify, request, send_from_directory

import gnmi_pb2
import gnmi_pb2_grpc

ROOT = Path(__file__).resolve().parent
PAYLOADS = ROOT / "payloads"
STATIC = ROOT / "static"
GNMI_TARGET = os.environ.get("GNMI_TARGET", "sonic-gnmi:8080")
REDIS_HOST = os.environ.get("REDIS_HOST", "redis-appl-db")
REDIS_PORT = int(os.environ.get("REDIS_PORT", "6379"))
APPL_DB = int(os.environ.get("APPL_DB", "0"))
LISTEN = os.environ.get("LISTEN", "0.0.0.0:8090")
ADAPTER_LOG = os.environ.get(
    "ADAPTER_LOG", "/var/log/syncd/sai-opi-adapter.log"
)

r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=APPL_DB, decode_responses=True)

# Same paths/files as scripts/run_sai_challenger.sh and run_iperf_overlay.sh.
ACTIONS = [
    {"id": "vnet", "group": "parents", "op": "set", "label": "VNET",
     "path": "/sonic-dash/DASH_VNET/DASH_VNET_LIST[vnet_name=Vnet1]", "file": "dash_vnet.json"},
    {"id": "qos", "group": "parents", "op": "set", "label": "QoS",
     "path": "/sonic-dash/DASH_QOS/DASH_QOS_LIST[qos_name=qos1]", "file": "dash_qos.json"},
    {"id": "route_group", "group": "parents", "op": "set", "label": "Route group",
     "path": "/sonic-dash/DASH_ROUTE_GROUP/DASH_ROUTE_GROUP_LIST[group_id=RouteGroup1]",
     "file": "dash_route_group.json"},
    {"id": "acl_group", "group": "parents", "op": "set", "label": "ACL group",
     "path": "/sonic-dash/DASH_ACL_GROUP/DASH_ACL_GROUP_LIST[group_id=acl-in-v4]",
     "file": "dash_acl_group.json"},
    {"id": "eni_a", "group": "children", "op": "set", "label": "ENI A",
     "path": "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=F4939FEFC47E]", "file": "dash_eni.json"},
    {"id": "eni_b", "group": "children", "op": "set", "label": "ENI B (same VNET)",
     "path": "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=001A2B3C4D5F]", "file": "dash_eni_b.json"},
    {"id": "route", "group": "children", "op": "set", "label": "Route 10.0.0.0/16",
     "path": "/sonic-dash/DASH_ROUTE/DASH_ROUTE_LIST[group_id=RouteGroup1][prefix=10.0.0.0/16]",
     "file": "dash_route.json"},
    {"id": "acl_http", "group": "children", "op": "set", "label": "ACL permit tcp/80",
     "path": "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=1]",
     "file": "dash_acl_rule.json"},
    {"id": "acl_iperf", "group": "children", "op": "set", "label": "ACL permit tcp/5201",
     "path": "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=2]",
     "file": "dash_acl_rule_iperf.json"},
    {"id": "del_acl_iperf", "group": "delete", "op": "delete", "label": "Delete ACL 5201",
     "path": "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=2]"},
    {"id": "del_acl_http", "group": "delete", "op": "delete", "label": "Delete ACL 80",
     "path": "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=1]"},
    {"id": "del_route", "group": "delete", "op": "delete", "label": "Delete route",
     "path": "/sonic-dash/DASH_ROUTE/DASH_ROUTE_LIST[group_id=RouteGroup1][prefix=10.0.0.0/16]"},
    {"id": "del_eni_b", "group": "delete", "op": "delete", "label": "Delete ENI B",
     "path": "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=001A2B3C4D5F]"},
    {"id": "del_eni_a", "group": "delete", "op": "delete", "label": "Delete ENI A",
     "path": "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=F4939FEFC47E]"},
    {"id": "del_acl_group", "group": "delete", "op": "delete", "label": "Delete ACL group",
     "path": "/sonic-dash/DASH_ACL_GROUP/DASH_ACL_GROUP_LIST[group_id=acl-in-v4]"},
    {"id": "del_route_group", "group": "delete", "op": "delete", "label": "Delete route group",
     "path": "/sonic-dash/DASH_ROUTE_GROUP/DASH_ROUTE_GROUP_LIST[group_id=RouteGroup1]"},
    {"id": "del_vnet", "group": "delete", "op": "delete", "label": "Delete VNET",
     "path": "/sonic-dash/DASH_VNET/DASH_VNET_LIST[vnet_name=Vnet1]"},
]

BUNDLES = {
    "create_4x4": {
        "label": "4×4 create (one ENI)",
        "steps": ["vnet", "qos", "route_group", "acl_group", "eni_a", "route", "acl_http"],
    },
    "create_iperf": {
        "label": "Two ENIs + route + ACL (iperf)",
        "steps": ["vnet", "qos", "route_group", "acl_group", "eni_a", "eni_b", "route",
                  "acl_http", "acl_iperf"],
    },
    "delete_4x4": {
        "label": "4×4 delete",
        "steps": ["del_acl_http", "del_route", "del_eni_a", "del_acl_group",
                  "del_route_group", "del_vnet"],
    },
    "delete_all": {
        "label": "Delete all (incl. ENI B)",
        "steps": ["del_acl_iperf", "del_acl_http", "del_route", "del_eni_b", "del_eni_a",
                  "del_acl_group", "del_route_group", "del_vnet"],
    },
}

BY_ID = {a["id"]: a for a in ACTIONS}

app = Flask(__name__, static_folder=str(STATIC), static_url_path="/static")


def parse_path(path: str) -> list[tuple[str, dict[str, str]]]:
    path = path.strip().lstrip("/")
    elems: list[tuple[str, dict[str, str]]] = []
    for part in path.split("/"):
        if not part:
            continue
        keys: dict[str, str] = {}
        name = part
        if "[" in part:
            name, rest = part.split("[", 1)
            rest = rest.rstrip("]")
            for kv in rest.split(","):
                k, _, v = kv.partition("=")
                if k:
                    keys[k] = v
        elems.append((name, keys))
    return elems


def fill_path(msg: gnmi_pb2.Path, path: str) -> None:
    for name, keys in parse_path(path):
        el = msg.elem.add()
        el.name = name
        for k, v in keys.items():
            el.key[k] = v


def gnmi_stub() -> gnmi_pb2_grpc.gNMIStub:
    host, port_s = GNMI_TARGET.rsplit(":", 1)
    ch = grpc.insecure_channel(f"{host}:{port_s}")
    return gnmi_pb2_grpc.gNMIStub(ch)


def run_set(path: str, body: dict) -> None:
    stub = gnmi_stub()
    req = gnmi_pb2.SetRequest()
    upd = req.replace.add()
    fill_path(upd.path, path)
    upd.val.json_ietf_val = json.dumps(body).encode("utf-8")
    stub.Set(req, timeout=10)


def run_delete(path: str) -> None:
    stub = gnmi_stub()
    req = gnmi_pb2.SetRequest()
    dpath = req.delete.add()
    fill_path(dpath, path)
    stub.Set(req, timeout=10)


def run_action(act: dict) -> dict:
    if act["op"] == "set":
        payload = json.loads((PAYLOADS / act["file"]).read_text())
        run_set(act["path"], payload)
        return {"ok": True, "op": "set", "path": act["path"]}
    run_delete(act["path"])
    return {"ok": True, "op": "delete", "path": act["path"]}


def appl_db() -> dict[str, dict]:
    out: dict[str, dict] = {}
    try:
        for key in sorted(r.scan_iter("DASH_*")):
            out[key] = r.hgetall(key)
    except redis.RedisError as exc:
        return {"_error": str(exc)}
    return out


def adapter_log(limit: int = 80) -> str:
    path = Path(ADAPTER_LOG)
    if not path.is_file():
        return ""
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return ""
    interesting = [
        ln for ln in lines
        if re.search(r"sai-opi:|Create|Delete|grpc_status", ln)
    ]
    use = interesting[-limit:] if interesting else lines[-limit:]
    return "\n".join(use)


def gnmi_up() -> bool:
    try:
        stub = gnmi_stub()
        stub.Capabilities(gnmi_pb2.CapabilityRequest(), timeout=2)
        return True
    except grpc.RpcError:
        return False


@app.get("/")
def index():
    return send_from_directory(STATIC, "index.html")


@app.get("/api/health")
def health():
    return jsonify({"ok": True, "gnmi": GNMI_TARGET})


@app.get("/api/catalog")
def catalog():
    return jsonify({"actions": ACTIONS, "bundles": BUNDLES, "gnmi": GNMI_TARGET})


@app.get("/api/status")
def status():
    return jsonify({
        "gnmi_up": gnmi_up(),
        "appl_db": appl_db(),
        "log": adapter_log(),
    })


@app.post("/api/action")
def api_action():
    body = request.get_json(silent=True) or {}
    aid = body.get("id")
    act = BY_ID.get(aid)
    if not act:
        return jsonify({"ok": False, "error": f"unknown action {aid}"}), 400
    try:
        return jsonify(run_action(act))
    except grpc.RpcError as exc:
        return jsonify({"ok": False, "error": exc.details() or str(exc)}), 502
    except OSError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 500


@app.post("/api/bundle")
def api_bundle():
    body = request.get_json(silent=True) or {}
    bid = body.get("id")
    bundle = BUNDLES.get(bid)
    if not bundle:
        return jsonify({"ok": False, "error": f"unknown bundle {bid}"}), 400
    results = []
    for step in bundle["steps"]:
        act = BY_ID[step]
        try:
            results.append({"id": step, **run_action(act)})
        except grpc.RpcError as exc:
            return jsonify({
                "ok": False,
                "error": f"{step}: {exc.details() or exc}",
                "done": results,
            }), 502
    return jsonify({"ok": True, "done": results})


def main() -> None:
    host, port_s = LISTEN.rsplit(":", 1)
    app.run(host=host, port=int(port_s), threaded=True)


if __name__ == "__main__":
    main()
