# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
"""
Lab sonic-gnmi: gNMI Set/Get/Capabilities on port 8080.

Maps DASH YANG-style JSON_IETF updates onto APPL_DB hashes defined in
sonic-net/SONiC doc/dash/dash-sonic-hld.md (DASH_*_TABLE). Official
docker-sonic-gnmi from sonic-buildimage is a drop-in replacement when
you have that image; this process is the host-portable subset.
"""

from __future__ import annotations

import json
import os
import re
import sys
from concurrent import futures

import grpc
import redis

import gnmi_pb2
import gnmi_pb2_grpc

REDIS_HOST = os.environ.get("REDIS_HOST", "redis-appl-db")
REDIS_PORT = int(os.environ.get("REDIS_PORT", "6379"))
APPL_DB = int(os.environ.get("APPL_DB", "0"))
LISTEN = os.environ.get("GNMI_LISTEN", "[::]:8080")

r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=APPL_DB, decode_responses=True)


def log(msg: str) -> None:
    print(f"sonic-gnmi: {msg}", flush=True)


def path_to_str(path: gnmi_pb2.Path) -> str:
    parts = []
    origin = getattr(path, "origin", "") or ""
    if origin:
        parts.append(origin)
    for elem in path.elem:
        bit = elem.name
        if elem.key:
            keys = ",".join(f"{k}={v}" for k, v in elem.key.items())
            bit = f"{bit}[{keys}]"
        parts.append(bit)
    return "/" + "/".join(parts)


def typed_json(val: gnmi_pb2.TypedValue):
    raw = val.json_ietf_val or val.json_val
    if not raw:
        if val.string_val:
            return val.string_val
        return None
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8")
    return json.loads(raw)


def parse_dash_target(path_s: str, body: dict) -> tuple[str, dict]:
    """Return (redis_key, hash fields) from a gNMI path + JSON body."""
    p = path_s.replace("/sonic-dash:", "/").replace("sonic-dash:", "")

    def kv(name: str) -> str | None:
        m = re.search(rf"{name}=([^,\]]+)", p)
        if m:
            return m.group(1)
        return body.get(name) or body.get("name")

    if "DASH_VNET" in p:
        name = kv("vnet_name") or kv("name") or body.get("vnet_name")
        if not name:
            raise ValueError(f"DASH_VNET missing name in {path_s}")
        fields = {
            "vni": str(body.get("vni", "")),
            "guid": str(body.get("guid", "")),
            "version": str(body.get("version", "1")),
        }
        return f"DASH_VNET_TABLE:{name}", fields

    if "DASH_QOS" in p:
        name = kv("qos_name") or kv("name") or body.get("qos_id")
        fields = {
            "qos_id": str(body.get("qos_id", name)),
            "cps": str(body.get("cps", "")),
            "flows": str(body.get("flows", "")),
            "bw": str(body.get("bw", "")),
        }
        return f"DASH_QOS_TABLE:{name}", fields

    if "DASH_ENI" in p:
        eni = kv("eni") or kv("eni_id") or body.get("eni") or body.get("mac_address", "")
        eni = str(eni).replace(":", "").replace("-", "").upper()
        fields = {
            "eni_id": str(body.get("eni_id", "")),
            "mac_address": str(body.get("mac_address", "")),
            "qos": str(body.get("qos", "")),
            "underlay_ip": str(body.get("underlay_ip", "")),
            "admin_state": str(body.get("admin_state", "enabled")),
            "vnet": str(body.get("vnet", "")),
            "mode": str(body.get("mode", "vm_mode")),
        }
        return f"DASH_ENI_TABLE:{eni}", fields

    if "DASH_ROUTE_GROUP" in p:
        gid = kv("group_id") or body.get("group_id")
        fields = {
            "guid": str(body.get("guid", "")),
            "version": str(body.get("version", "1")),
        }
        return f"DASH_ROUTE_GROUP_TABLE:{gid}", fields

    if "DASH_ROUTE" in p and "ROUTE_GROUP" not in p:
        gid = kv("group_id") or body.get("group_id")
        prefix = kv("prefix") or body.get("prefix")
        fields = {
            "action_type": str(body.get("action_type", body.get("routing_type", "vnet"))),
            "vnet": str(body.get("vnet", "")),
        }
        return f"DASH_ROUTE_TABLE:{gid}:{prefix}", fields

    if "DASH_ACL_GROUP" in p:
        gid = kv("group_id") or body.get("group_id")
        fields = {
            "ip_version": str(body.get("ip_version", "ipv4")),
            "guid": str(body.get("guid", "")),
        }
        return f"DASH_ACL_GROUP_TABLE:{gid}", fields

    if "DASH_ACL_RULE" in p:
        gid = kv("group_id") or body.get("group_id")
        rule = kv("rule_num") or body.get("rule_num")
        fields = {
            "priority": str(body.get("priority", "1")),
            "action": str(body.get("action", "permit")),
            "terminating": str(body.get("terminating", "true")),
            "protocol": str(body.get("protocol", "")),
            "src_addr": str(body.get("src_addr", "")),
            "dst_addr": str(body.get("dst_addr", "")),
            "src_port": str(body.get("src_port", "")),
            "dst_port": str(body.get("dst_port", "")),
        }
        return f"DASH_ACL_RULE_TABLE:{gid}:{rule}", fields

    raise ValueError(f"unsupported DASH gNMI path: {path_s}")


def apply_update(path: gnmi_pb2.Path, val: gnmi_pb2.TypedValue) -> str:
    path_s = path_to_str(path)
    body = typed_json(val)
    if body is None:
        raise ValueError(f"empty value for {path_s}")
    if not isinstance(body, dict):
        raise ValueError(f"JSON_IETF object required for {path_s}")
    key, fields = parse_dash_target(path_s, body)
    fields = {k: v for k, v in fields.items() if v != ""}
    if not fields:
        raise ValueError(f"no fields for {key}")
    r.hset(key, mapping=fields)
    log(f"APPL_DB HSET {key} {fields}")
    return key


class GnmiServicer(gnmi_pb2_grpc.gNMIServicer):
    def Capabilities(self, request, context):
        log("Capabilities")
        m = gnmi_pb2.ModelData(name="sonic-dash", organization="sonic-net", version="1.0")
        return gnmi_pb2.CapabilityResponse(
            supported_models=[m],
            gNMI_version="0.10.0",
        )

    def Get(self, request, context):
        notifications = []
        prefix = request.prefix
        for p in request.path:
            full = gnmi_pb2.Path()
            full.CopyFrom(prefix)
            full.elem.extend(p.elem)
            path_s = path_to_str(full)
            try:
                key, _ = parse_dash_target(path_s, {})
            except ValueError:
                continue
            data = r.hgetall(key)
            upd = gnmi_pb2.Update(
                path=full,
                val=gnmi_pb2.TypedValue(
                    json_ietf_val=json.dumps(data).encode("utf-8")
                ),
            )
            notifications.append(
                gnmi_pb2.Notification(update=[upd])
            )
        return gnmi_pb2.GetResponse(notification=notifications)

    def Set(self, request, context):
        results = []
        prefix = request.prefix
        for upd in list(request.replace) + list(request.update):
            path = gnmi_pb2.Path()
            path.CopyFrom(prefix)
            path.elem.extend(upd.path.elem)
            try:
                key = apply_update(path, upd.val)
                results.append(
                    gnmi_pb2.UpdateResult(
                        path=path,
                        op=gnmi_pb2.UpdateResult.UPDATE,
                    )
                )
                log(f"Set ok {key}")
            except Exception as exc:  # noqa: BLE001
                log(f"Set failed: {exc}")
                context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
                context.set_details(str(exc))
                return gnmi_pb2.SetResponse()
        for dpath in request.delete:
            path = gnmi_pb2.Path()
            path.CopyFrom(prefix)
            path.elem.extend(dpath.elem)
            path_s = path_to_str(path)
            try:
                key, _ = parse_dash_target(path_s, {})
                r.delete(key)
                log(f"APPL_DB DEL {key}")
                results.append(
                    gnmi_pb2.UpdateResult(path=path, op=gnmi_pb2.UpdateResult.DELETE)
                )
            except Exception as exc:  # noqa: BLE001
                log(f"Delete skip {path_s}: {exc}")
        return gnmi_pb2.SetResponse(response=results)


def main() -> None:
    for attempt in range(30):
        try:
            r.ping()
            break
        except redis.ConnectionError:
            import time

            time.sleep(0.5)
    else:
        log(f"cannot reach redis {REDIS_HOST}:{REDIS_PORT}")
        sys.exit(1)

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
    gnmi_pb2_grpc.add_gNMIServicer_to_server(GnmiServicer(), server)
    server.add_insecure_port(LISTEN)
    server.start()
    log(f"listening on {LISTEN} (APPL_DB redis://{REDIS_HOST}:{REDIS_PORT}/{APPL_DB})")
    server.wait_for_termination()


if __name__ == "__main__":
    main()
