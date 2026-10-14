# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
"""
Lab dashorch: consume APPL_DB DASH tables and enqueue SAI C-API work for syncd.

Mirrors sonic-swss DashOrch: VNET before ENI, route groups before routes,
ACL groups before rules. Official docker-orchagent is a drop-in when you
have a DASH-enabled SONiC swss image.
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import time

import redis

REDIS_HOST = os.environ.get("REDIS_HOST", "redis-appl-db")
APPL = int(os.environ.get("APPL_DB", "0"))
ASIC = int(os.environ.get("ASIC_DB", "1"))
STATE = int(os.environ.get("STATE_DB", "6"))
WORKQ = os.environ.get("SAI_WORKQ", "SAI_OPI:WORKQ")
SWITCH_ID = int(os.environ.get("SAI_SWITCH_ID", "0x21000000000000"), 0)

# Redis keyspace events when a key is gone (gNMI Set delete → DEL).
_DELETE_EVENTS = frozenset({"del", "expired", "evicted"})

# redis-py 8+ defaults socket_timeout=5, which aborts pubsub.listen() on idle.
_redis_kw = dict(
    host=REDIS_HOST,
    decode_responses=True,
    socket_connect_timeout=5,
    socket_timeout=None,
)
appl = redis.Redis(db=APPL, **_redis_kw)
asic = redis.Redis(db=ASIC, **_redis_kw)
state = redis.Redis(db=STATE, **_redis_kw)

_seen: set[str] = set()
_lock = threading.Lock()


def log(msg: str) -> None:
    print(f"dashorch: {msg}", flush=True)


def vnet_oid(name: str) -> int:
    key = f"DASH_OID_MAP|vnet|{name}"
    existing = state.get(key)
    if existing:
        return int(existing)
    digest = hashlib.sha256(name.encode("utf-8")).digest()
    local = int.from_bytes(digest[:6], "big")
    oid = (0x0E << 48) | local
    state.set(key, str(oid))
    return oid


def enqueue(op: dict) -> None:
    asic.lpush(WORKQ, json.dumps(op))
    log(f"ASIC_DB {WORKQ} <- {op.get('op')} {op.get('key', '')}")


def forget_key(key: str) -> None:
    """Drop create signatures so a later recreate of the same key is processed."""
    with _lock:
        for sig in [s for s in _seen if s.startswith(key)]:
            _seen.discard(sig)


def handle_delete(key: str) -> None:
    if not key.startswith("DASH_"):
        return
    forget_key(key)
    log(f"APPL_DB delete {key}")
    if key.startswith("DASH_ENI_TABLE:"):
        enqueue({"op": "remove_eni", "key": key})
        return
    if key.startswith("DASH_VNET_TABLE:"):
        enqueue({"op": "remove_vnet", "key": key, "vnet": key.split(":", 1)[1]})
        return
    if key.startswith("DASH_ROUTE_GROUP_TABLE:"):
        group_id = key.split(":", 1)[1]
        enqueue({"op": "remove_route_group", "key": key, "group_id": group_id})
        return
    if key.startswith("DASH_ROUTE_TABLE:"):
        # DASH_ROUTE_TABLE:{group_id}:{prefix}
        parts = key.split(":", 2)
        group_id = parts[1] if len(parts) > 1 else ""
        prefix = parts[2] if len(parts) > 2 else ""
        enqueue(
            {
                "op": "remove_route",
                "key": key,
                "group_id": group_id,
                "prefix": prefix,
            }
        )
        return
    if key.startswith("DASH_ACL_GROUP_TABLE:"):
        group_id = key.split(":", 1)[1]
        enqueue({"op": "remove_acl_group", "key": key, "group_id": group_id})
        return
    if key.startswith("DASH_ACL_RULE_TABLE:"):
        parts = key.split(":", 2)
        group_id = parts[1] if len(parts) > 1 else ""
        rule_num = parts[2] if len(parts) > 2 else ""
        enqueue(
            {
                "op": "remove_acl_rule",
                "key": key,
                "group_id": group_id,
                "rule_num": rule_num,
            }
        )
        return
    log(f"delete ignored (no SAI remove) {key}")


def handle_key(key: str) -> None:
    if not key.startswith("DASH_"):
        return
    fields = appl.hgetall(key)
    if not fields:
        # Key already gone (race with DEL) — remove path handles it via event.
        return
    with _lock:
        sig = key + json.dumps(fields, sort_keys=True)
        if sig in _seen:
            return
        _seen.add(sig)

    log(f"APPL_DB change {key} {fields}")

    if key.startswith("DASH_VNET_TABLE:"):
        name = key.split(":", 1)[1]
        enqueue(
            {
                "op": "create_vnet",
                "key": key,
                "vnet": name,
                "vni": int(fields.get("vni") or 0),
                "switch_id": SWITCH_ID,
                "vpc": f"vpcs/{name}",
                "subnet": f"subnets/{name}-default",
            }
        )
        return

    if key.startswith("DASH_ENI_TABLE:"):
        vnet = fields.get("vnet", "")
        # Prefer SAI oid written by syncd after create_vnet; else stable hash.
        oid = 0
        if vnet:
            mapped = asic.hget(f"SAI_OPI:VNET_OID|{vnet}", "vnet_oid")
            if mapped:
                oid = int(mapped, 0)
            else:
                oid = vnet_oid(vnet)
        mac = fields.get("mac_address", "").replace("-", ":")
        if mac and ":" not in mac and len(mac) == 12:
            mac = ":".join(mac[i : i + 2] for i in range(0, 12, 2))
        qos = fields.get("qos", "")
        cps = 0
        if qos:
            cps = int(appl.hget(f"DASH_QOS_TABLE:{qos}", "cps") or 0)
        enqueue(
            {
                "op": "create_eni",
                "key": key,
                "switch_id": SWITCH_ID,
                "vnet": vnet,
                "vnet_oid": oid,
                "cps": cps,
                "mac": mac.lower(),
                "admin": 1 if fields.get("admin_state", "enabled") == "enabled" else 0,
                "vm_vni": int(appl.hget(f"DASH_VNET_TABLE:{vnet}", "vni") or 0),
                "eni_id": fields.get("eni_id", ""),
                "mode": fields.get("mode", "vm_mode"),
            }
        )
        return

    if key.startswith("DASH_ROUTE_GROUP_TABLE:"):
        group_id = key.split(":", 1)[1]
        enqueue(
            {
                "op": "create_route_group",
                "key": key,
                "group_id": group_id,
                "switch_id": SWITCH_ID,
                "fields": fields,
            }
        )
        return

    if key.startswith("DASH_ROUTE_TABLE:"):
        # DASH_ROUTE_TABLE:{group_id}:{prefix}
        parts = key.split(":", 2)
        group_id = parts[1] if len(parts) > 1 else fields.get("group_id", "")
        prefix = parts[2] if len(parts) > 2 else fields.get("prefix", "")
        enqueue(
            {
                "op": "create_route",
                "key": key,
                "group_id": group_id,
                "prefix": prefix,
                "action_type": fields.get("action_type", "vnet"),
                "vnet": fields.get("vnet", ""),
                "switch_id": SWITCH_ID,
                "fields": fields,
            }
        )
        return

    if key.startswith("DASH_ACL_GROUP_TABLE:"):
        group_id = key.split(":", 1)[1]
        enqueue(
            {
                "op": "create_acl_group",
                "key": key,
                "group_id": group_id,
                "ip_version": fields.get("ip_version", "ipv4"),
                "switch_id": SWITCH_ID,
                "fields": fields,
            }
        )
        return

    if key.startswith("DASH_ACL_RULE_TABLE:"):
        parts = key.split(":", 2)
        group_id = parts[1] if len(parts) > 1 else fields.get("group_id", "")
        rule_num = parts[2] if len(parts) > 2 else fields.get("rule_num", "")
        enqueue(
            {
                "op": "create_acl_rule",
                "key": key,
                "group_id": group_id,
                "rule_num": rule_num,
                "priority": int(fields.get("priority") or 1),
                "action": fields.get("action", "permit"),
                "terminating": fields.get("terminating", "true"),
                "protocol": fields.get("protocol", ""),
                "src_addr": fields.get("src_addr", ""),
                "dst_addr": fields.get("dst_addr", ""),
                "src_port": fields.get("src_port", ""),
                "dst_port": fields.get("dst_port", ""),
                "switch_id": SWITCH_ID,
                "fields": fields,
            }
        )
        return

    if key.startswith("DASH_QOS_TABLE:"):
        log(f"QoS cached for ENI cps lookup {key}")
        return


def scan_existing() -> None:
    for pattern in (
        "DASH_VNET_TABLE:*",
        "DASH_QOS_TABLE:*",
        "DASH_ROUTE_GROUP_TABLE:*",
        "DASH_ACL_GROUP_TABLE:*",
        "DASH_ENI_TABLE:*",
        "DASH_ROUTE_TABLE:*",
        "DASH_ACL_RULE_TABLE:*",
    ):
        for key in appl.scan_iter(pattern):
            handle_key(key)


def main() -> None:
    for _ in range(40):
        try:
            appl.ping()
            break
        except redis.ConnectionError:
            time.sleep(0.5)
    log(f"watching APPL_DB on {REDIS_HOST}")
    scan_existing()
    pub = appl.pubsub()
    pub.psubscribe("__keyspace@0__:DASH_*")
    for msg in pub.listen():
        if msg.get("type") not in ("pmessage", "message"):
            continue
        chan = str(msg.get("channel") or "")
        # __keyspace@0__:DASH_VNET_TABLE:Vnet1
        key = chan.split(":", 1)[-1]
        event = str(msg.get("data") or "").lower()
        if event in _DELETE_EVENTS:
            handle_delete(key)
        else:
            handle_key(key)


if __name__ == "__main__":
    main()
