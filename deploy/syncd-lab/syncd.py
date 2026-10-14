# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
"""
Lab syncd: pop SAI C-API work from ASIC_DB and invoke libsai_opi_adapter.so.
"""

from __future__ import annotations

import ctypes
import json
import os
import time

import redis

REDIS_HOST = os.environ.get("REDIS_HOST", "redis-appl-db")
ASIC = int(os.environ.get("ASIC_DB", "1"))
WORKQ = os.environ.get("SAI_WORKQ", "SAI_OPI:WORKQ")
OPI = os.environ.get("OPI_SERVER_ADDRESS", "host.docker.internal:50051")
LIB = os.environ.get("LAB_SAI_SO", "/usr/local/lib/liblab_sai.so")

# redis-py 8+ defaults socket_timeout=5, which races BRPOP timeout=5 and
# kills the worker on an empty queue. Blocking reads must stay unbounded.
asic = redis.Redis(
    host=REDIS_HOST,
    db=ASIC,
    decode_responses=True,
    socket_connect_timeout=5,
    socket_timeout=None,
)


def log(msg: str) -> None:
    print(f"syncd-lab: {msg}", flush=True)


def eni_oid_key(appl_key: str) -> str:
    return f"SAI_OPI:ENI:{appl_key}"


def vnet_oid_key(vnet_name: str) -> str:
    return f"SAI_OPI:VNET_OID|{vnet_name}"


def route_group_oid_key(group_id: str) -> str:
    return f"SAI_OPI:ROUTE_GROUP_OID|{group_id}"


def acl_group_oid_key(group_id: str) -> str:
    return f"SAI_OPI:ACL_GROUP_OID|{group_id}"


def acl_rule_oid_key(group_id: str, rule_num: str) -> str:
    return f"SAI_OPI:ACL_RULE_OID|{group_id}|{rule_num}"


def load_lab() -> ctypes.CDLL | None:
    try:
        lib = ctypes.CDLL(LIB)
    except OSError as exc:
        log(f"cannot load {LIB}: {exc}")
        return None
    lib.lab_sai_init.argtypes = [ctypes.c_char_p]
    lib.lab_sai_init.restype = ctypes.c_int
    lib.lab_sai_bind_vnet.argtypes = [
        ctypes.c_uint64,
        ctypes.c_char_p,
        ctypes.c_char_p,
    ]
    lib.lab_sai_bind_vnet.restype = ctypes.c_int
    lib.lab_sai_create_eni.argtypes = [
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint32,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_uint32,
    ]
    lib.lab_sai_create_eni.restype = ctypes.c_int
    lib.lab_sai_remove_eni.argtypes = [ctypes.c_uint64]
    lib.lab_sai_remove_eni.restype = ctypes.c_int
    lib.lab_sai_create_vnet.argtypes = [
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.c_uint64,
        ctypes.c_uint32,
    ]
    lib.lab_sai_create_vnet.restype = ctypes.c_int
    lib.lab_sai_remove_vnet.argtypes = [ctypes.c_uint64]
    lib.lab_sai_remove_vnet.restype = ctypes.c_int
    lib.lab_sai_create_route_group.argtypes = [
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.c_uint64,
        ctypes.c_int,
    ]
    lib.lab_sai_create_route_group.restype = ctypes.c_int
    lib.lab_sai_remove_route_group.argtypes = [ctypes.c_uint64]
    lib.lab_sai_remove_route_group.restype = ctypes.c_int
    lib.lab_sai_create_route.argtypes = [
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_uint64,
    ]
    lib.lab_sai_create_route.restype = ctypes.c_int
    lib.lab_sai_remove_route.argtypes = [
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_char_p,
    ]
    lib.lab_sai_remove_route.restype = ctypes.c_int
    lib.lab_sai_create_acl_group.argtypes = [
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.c_uint64,
        ctypes.c_int,
    ]
    lib.lab_sai_create_acl_group.restype = ctypes.c_int
    lib.lab_sai_remove_acl_group.argtypes = [ctypes.c_uint64]
    lib.lab_sai_remove_acl_group.restype = ctypes.c_int
    lib.lab_sai_create_acl_rule.argtypes = [
        ctypes.POINTER(ctypes.c_uint64),
        ctypes.c_uint64,
        ctypes.c_uint64,
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
    ]
    lib.lab_sai_create_acl_rule.restype = ctypes.c_int
    lib.lab_sai_remove_acl_rule.argtypes = [ctypes.c_uint64]
    lib.lab_sai_remove_acl_rule.restype = ctypes.c_int
    return lib


def resolve_eni_oid(appl_key: str) -> int | None:
    oid_s = asic.hget(eni_oid_key(appl_key), "eni_oid")
    if not oid_s and appl_key:
        last_key = asic.hget("SAI_OPI:LAST_ENI", "key")
        last_st = asic.hget("SAI_OPI:LAST_ENI", "status")
        if last_key == appl_key and last_st == "0":
            oid_s = asic.hget("SAI_OPI:LAST_ENI", "eni_oid")
    if not oid_s:
        return None
    try:
        n = int(oid_s, 0)
    except ValueError:
        return None
    if n == 0:
        return None
    return n


def resolve_vnet_oid(vnet_name: str) -> int | None:
    oid_s = asic.hget(vnet_oid_key(vnet_name), "vnet_oid")
    if not oid_s:
        return None
    try:
        return int(oid_s, 0)
    except ValueError:
        return None


def resolve_route_group_oid(group_id: str) -> int | None:
    oid_s = asic.hget(route_group_oid_key(group_id), "group_oid")
    if not oid_s:
        return None
    try:
        return int(oid_s, 0)
    except ValueError:
        return None


def resolve_acl_group_oid(group_id: str) -> int | None:
    oid_s = asic.hget(acl_group_oid_key(group_id), "group_oid")
    if not oid_s:
        return None
    try:
        return int(oid_s, 0)
    except ValueError:
        return None


def resolve_acl_rule_oid(group_id: str, rule_num: str) -> int | None:
    oid_s = asic.hget(acl_rule_oid_key(group_id, rule_num), "rule_oid")
    if not oid_s:
        return None
    try:
        return int(oid_s, 0)
    except ValueError:
        return None


def route_action(action_type: str) -> int:
    # Matches sai_outbound_routing_entry_action_t in the stub header.
    return {
        "vnet": 0,
        "vnet_direct": 1,
        "direct": 2,
        "service_tunnel": 3,
        "drop": 4,
    }.get((action_type or "vnet").lower(), 0)


def acl_action(action: str, terminating: str) -> int:
    # sai_dash_acl_rule_action_t
    a = (action or "permit").lower()
    term = str(terminating or "true").lower() in ("1", "true", "yes")
    if a in ("deny", "drop"):
        return 2 if term else 3  # DENY / DENY_AND_CONTINUE
    if a in ("permit_and_continue", "allow_and_continue"):
        return 1
    if a in ("deny_and_continue",):
        return 3
    return 0 if term else 1  # PERMIT / PERMIT_AND_CONTINUE


def main() -> None:
    for _ in range(40):
        try:
            asic.ping()
            break
        except redis.ConnectionError:
            time.sleep(0.5)

    lib = load_lab()
    if lib is None:
        log("running in log-only mode (adapter not loaded)")
    else:
        st = lib.lab_sai_init(OPI.encode("utf-8"))
        log(f"dash_opi_client_init({OPI}) status={st}")
        if st != 0:
            log("OPI channel failed — SAI creates will return errors")

    log(f"BRPOP {WORKQ} on ASIC_DB")
    while True:
        try:
            item = asic.brpop(WORKQ, timeout=5)
        except (TimeoutError, redis.TimeoutError, redis.ConnectionError) as exc:
            log(f"BRPOP retry after {type(exc).__name__}: {exc}")
            time.sleep(0.2)
            continue
        if not item:
            continue
        _, raw = item
        try:
            op = json.loads(raw)
        except json.JSONDecodeError:
            log(f"bad work item {raw!r}")
            continue
        kind = op.get("op")
        log(f"SAI op={kind} key={op.get('key', '')}")

        if kind == "bind_vnet" and lib:
            # Legacy ObjectStore-only bind (kept for older work items).
            st = lib.lab_sai_bind_vnet(
                int(op["vnet_oid"]),
                op["vpc"].encode(),
                op["subnet"].encode(),
            )
            log(f"dash_opi_bind_vnet {op.get('vnet')} vpc={op.get('vpc')} status={st}")
            asic.hset(
                "SAI_OPI:LAST_VNET",
                mapping={
                    "status": str(st),
                    **{k: str(v) for k, v in op.items() if k != "op"},
                },
            )

        elif kind == "create_vnet" and lib:
            vnet = ctypes.c_uint64(0)
            st = lib.lab_sai_create_vnet(
                ctypes.byref(vnet),
                int(op.get("switch_id") or 0),
                int(op.get("vni") or 0),
            )
            name = op.get("vnet", "")
            log(
                f"sai_create_vnet name={name} vni={op.get('vni')} "
                f"status={st} vnet_oid=0x{vnet.value:x}"
            )
            mapping = {
                "status": str(st),
                "vnet_oid": hex(vnet.value),
                "vnet": name,
                "vni": str(op.get("vni") or 0),
                "key": op.get("key", ""),
            }
            asic.hset("SAI_OPI:LAST_VNET", mapping=mapping)
            if name and st == 0:
                asic.hset(vnet_oid_key(name), mapping=mapping)

        elif kind == "remove_vnet" and lib:
            name = op.get("vnet") or ""
            if not name and op.get("key", "").startswith("DASH_VNET_TABLE:"):
                name = op["key"].split(":", 1)[1]
            vnet_oid = resolve_vnet_oid(name)
            if vnet_oid is None:
                log(f"sai_remove_vnet missing oid for {name}")
                asic.hset(
                    "SAI_OPI:LAST_VNET_REMOVE",
                    mapping={"status": "missing_oid", "vnet": name},
                )
            else:
                st = lib.lab_sai_remove_vnet(vnet_oid)
                log(f"sai_remove_vnet name={name} oid=0x{vnet_oid:x} status={st}")
                asic.hset(
                    "SAI_OPI:LAST_VNET_REMOVE",
                    mapping={
                        "status": str(st),
                        "vnet_oid": hex(vnet_oid),
                        "vnet": name,
                    },
                )
                if name:
                    asic.delete(vnet_oid_key(name))

        elif kind == "create_eni" and lib:
            vnet_oid = int(op.get("vnet_oid") or 0)
            if not vnet_oid and op.get("vnet"):
                mapped = resolve_vnet_oid(op["vnet"])
                if mapped is not None:
                    vnet_oid = mapped
            eni = ctypes.c_uint64(0)
            st = lib.lab_sai_create_eni(
                ctypes.byref(eni),
                int(op["switch_id"]),
                vnet_oid,
                int(op.get("cps") or 0),
                op.get("mac", "").encode(),
                int(op.get("admin") or 0),
                int(op.get("vm_vni") or 0),
            )
            log(
                f"sai_create_eni mac={op.get('mac')} vnet={op.get('vnet')} "
                f"status={st} eni_oid=0x{eni.value:x}"
            )
            mapping = {
                "status": str(st),
                "eni_oid": hex(eni.value),
                "mac": op.get("mac", ""),
                "key": op.get("key", ""),
            }
            asic.hset("SAI_OPI:LAST_ENI", mapping=mapping)
            if op.get("key") and st == 0:
                asic.hset(eni_oid_key(op["key"]), mapping=mapping)

        elif kind == "remove_eni" and lib:
            appl_key = op.get("key", "")
            eni_oid = resolve_eni_oid(appl_key)
            if eni_oid is None:
                log(f"sai_remove_eni missing oid for {appl_key}")
                asic.hset(
                    "SAI_OPI:LAST_ENI_REMOVE",
                    mapping={"status": "missing_oid", "key": appl_key},
                )
            else:
                st = lib.lab_sai_remove_eni(eni_oid)
                log(f"sai_remove_eni key={appl_key} eni_oid=0x{eni_oid:x} status={st}")
                asic.hset(
                    "SAI_OPI:LAST_ENI_REMOVE",
                    mapping={
                        "status": str(st),
                        "eni_oid": hex(eni_oid),
                        "key": appl_key,
                    },
                )
                if appl_key:
                    asic.delete(eni_oid_key(appl_key))

        elif kind == "create_route_group" and lib:
            group = ctypes.c_uint64(0)
            st = lib.lab_sai_create_route_group(
                ctypes.byref(group),
                int(op.get("switch_id") or 0),
                0,
            )
            group_id = op.get("group_id") or ""
            if not group_id and op.get("key", "").startswith("DASH_ROUTE_GROUP_TABLE:"):
                group_id = op["key"].split(":", 1)[1]
            log(
                f"sai_create_outbound_routing_group group_id={group_id} "
                f"status={st} group_oid=0x{group.value:x}"
            )
            mapping = {
                "status": str(st),
                "group_oid": hex(group.value),
                "group_id": group_id,
                "key": op.get("key", ""),
            }
            asic.hset("SAI_OPI:LAST_ROUTE_GROUP", mapping=mapping)
            if group_id and st == 0:
                asic.hset(route_group_oid_key(group_id), mapping=mapping)

        elif kind == "remove_route_group" and lib:
            group_id = op.get("group_id") or ""
            if not group_id and op.get("key", "").startswith("DASH_ROUTE_GROUP_TABLE:"):
                group_id = op["key"].split(":", 1)[1]
            group_oid = resolve_route_group_oid(group_id)
            if group_oid is None:
                log(f"sai_remove_outbound_routing_group missing oid for {group_id}")
                asic.hset(
                    "SAI_OPI:LAST_ROUTE_GROUP_REMOVE",
                    mapping={"status": "missing_oid", "group_id": group_id},
                )
            else:
                st = lib.lab_sai_remove_route_group(group_oid)
                log(
                    f"sai_remove_outbound_routing_group group_id={group_id} "
                    f"oid=0x{group_oid:x} status={st}"
                )
                asic.hset(
                    "SAI_OPI:LAST_ROUTE_GROUP_REMOVE",
                    mapping={
                        "status": str(st),
                        "group_oid": hex(group_oid),
                        "group_id": group_id,
                    },
                )
                if group_id:
                    asic.delete(route_group_oid_key(group_id))

        elif kind == "create_route" and lib:
            group_id = op.get("group_id") or ""
            prefix = op.get("prefix") or ""
            if (not group_id or not prefix) and op.get("key", "").startswith(
                "DASH_ROUTE_TABLE:"
            ):
                parts = op["key"].split(":", 2)
                if len(parts) > 1 and not group_id:
                    group_id = parts[1]
                if len(parts) > 2 and not prefix:
                    prefix = parts[2]
            group_oid = resolve_route_group_oid(group_id)
            if group_oid is None:
                log(f"sai_create_outbound_routing_entry missing group oid for {group_id}")
                asic.hset(
                    "SAI_OPI:LAST_ROUTE",
                    mapping={
                        "status": "missing_group_oid",
                        "group_id": group_id,
                        "prefix": prefix,
                        "key": op.get("key", ""),
                    },
                )
            else:
                vnet_name = op.get("vnet") or ""
                dst_vnet = resolve_vnet_oid(vnet_name) if vnet_name else 0
                if dst_vnet is None:
                    dst_vnet = 0
                st = lib.lab_sai_create_route(
                    int(op.get("switch_id") or 0),
                    group_oid,
                    prefix.encode(),
                    route_action(str(op.get("action_type") or "vnet")),
                    int(dst_vnet),
                )
                log(
                    f"sai_create_outbound_routing_entry group={group_id} "
                    f"prefix={prefix} vnet={vnet_name} status={st}"
                )
                asic.hset(
                    "SAI_OPI:LAST_ROUTE",
                    mapping={
                        "status": str(st),
                        "group_id": group_id,
                        "group_oid": hex(group_oid),
                        "prefix": prefix,
                        "vnet": vnet_name,
                        "key": op.get("key", ""),
                    },
                )

        elif kind == "remove_route" and lib:
            group_id = op.get("group_id") or ""
            prefix = op.get("prefix") or ""
            if (not group_id or not prefix) and op.get("key", "").startswith(
                "DASH_ROUTE_TABLE:"
            ):
                parts = op["key"].split(":", 2)
                if len(parts) > 1 and not group_id:
                    group_id = parts[1]
                if len(parts) > 2 and not prefix:
                    prefix = parts[2]
            group_oid = resolve_route_group_oid(group_id)
            if group_oid is None:
                log(f"sai_remove_outbound_routing_entry missing group oid for {group_id}")
                asic.hset(
                    "SAI_OPI:LAST_ROUTE_REMOVE",
                    mapping={
                        "status": "missing_group_oid",
                        "group_id": group_id,
                        "prefix": prefix,
                    },
                )
            else:
                st = lib.lab_sai_remove_route(
                    int(op.get("switch_id") or 0),
                    group_oid,
                    prefix.encode(),
                )
                log(
                    f"sai_remove_outbound_routing_entry group={group_id} "
                    f"prefix={prefix} status={st}"
                )
                asic.hset(
                    "SAI_OPI:LAST_ROUTE_REMOVE",
                    mapping={
                        "status": str(st),
                        "group_id": group_id,
                        "prefix": prefix,
                    },
                )

        elif kind == "create_acl_group" and lib:
            group = ctypes.c_uint64(0)
            ipv6 = 1 if str(op.get("ip_version") or "").lower() in ("ipv6", "v6") else 0
            st = lib.lab_sai_create_acl_group(
                ctypes.byref(group),
                int(op.get("switch_id") or 0),
                ipv6,
            )
            group_id = op.get("group_id") or ""
            if not group_id and op.get("key", "").startswith("DASH_ACL_GROUP_TABLE:"):
                group_id = op["key"].split(":", 1)[1]
            log(
                f"sai_create_dash_acl_group group_id={group_id} "
                f"status={st} group_oid=0x{group.value:x}"
            )
            mapping = {
                "status": str(st),
                "group_oid": hex(group.value),
                "group_id": group_id,
                "key": op.get("key", ""),
            }
            asic.hset("SAI_OPI:LAST_ACL_GROUP", mapping=mapping)
            if group_id and st == 0:
                asic.hset(acl_group_oid_key(group_id), mapping=mapping)

        elif kind == "remove_acl_group" and lib:
            group_id = op.get("group_id") or ""
            if not group_id and op.get("key", "").startswith("DASH_ACL_GROUP_TABLE:"):
                group_id = op["key"].split(":", 1)[1]
            group_oid = resolve_acl_group_oid(group_id)
            if group_oid is None:
                log(f"sai_remove_dash_acl_group missing oid for {group_id}")
                asic.hset(
                    "SAI_OPI:LAST_ACL_GROUP_REMOVE",
                    mapping={"status": "missing_oid", "group_id": group_id},
                )
            else:
                st = lib.lab_sai_remove_acl_group(group_oid)
                log(
                    f"sai_remove_dash_acl_group group_id={group_id} "
                    f"oid=0x{group_oid:x} status={st}"
                )
                asic.hset(
                    "SAI_OPI:LAST_ACL_GROUP_REMOVE",
                    mapping={
                        "status": str(st),
                        "group_oid": hex(group_oid),
                        "group_id": group_id,
                    },
                )
                if group_id:
                    asic.delete(acl_group_oid_key(group_id))

        elif kind == "create_acl_rule" and lib:
            group_id = op.get("group_id") or ""
            rule_num = str(op.get("rule_num") or "")
            if (not group_id or not rule_num) and op.get("key", "").startswith(
                "DASH_ACL_RULE_TABLE:"
            ):
                parts = op["key"].split(":", 2)
                if len(parts) > 1 and not group_id:
                    group_id = parts[1]
                if len(parts) > 2 and not rule_num:
                    rule_num = parts[2]
            group_oid = resolve_acl_group_oid(group_id)
            if group_oid is None:
                log(f"sai_create_dash_acl_rule missing group oid for {group_id}")
                asic.hset(
                    "SAI_OPI:LAST_ACL",
                    mapping={
                        "status": "missing_group_oid",
                        "group_id": group_id,
                        "rule_num": rule_num,
                        "key": op.get("key", ""),
                    },
                )
            else:
                proto_s = str(op.get("protocol") or "").strip()
                protocol = int(proto_s) if proto_s.isdigit() else 0
                rule = ctypes.c_uint64(0)
                st = lib.lab_sai_create_acl_rule(
                    ctypes.byref(rule),
                    int(op.get("switch_id") or 0),
                    group_oid,
                    int(op.get("priority") or 1),
                    acl_action(str(op.get("action") or "permit"), str(op.get("terminating") or "true")),
                    protocol,
                    str(op.get("src_addr") or "0.0.0.0/0").encode(),
                    str(op.get("dst_addr") or "0.0.0.0/0").encode(),
                    str(op.get("src_port") or "0-65535").encode(),
                    str(op.get("dst_port") or "0-65535").encode(),
                )
                log(
                    f"sai_create_dash_acl_rule group={group_id} rule={rule_num} "
                    f"status={st} rule_oid=0x{rule.value:x}"
                )
                mapping = {
                    "status": str(st),
                    "group_id": group_id,
                    "rule_num": rule_num,
                    "rule_oid": hex(rule.value),
                    "key": op.get("key", ""),
                }
                asic.hset("SAI_OPI:LAST_ACL", mapping=mapping)
                if group_id and rule_num and st == 0:
                    asic.hset(acl_rule_oid_key(group_id, rule_num), mapping=mapping)

        elif kind == "remove_acl_rule" and lib:
            group_id = op.get("group_id") or ""
            rule_num = str(op.get("rule_num") or "")
            if (not group_id or not rule_num) and op.get("key", "").startswith(
                "DASH_ACL_RULE_TABLE:"
            ):
                parts = op["key"].split(":", 2)
                if len(parts) > 1 and not group_id:
                    group_id = parts[1]
                if len(parts) > 2 and not rule_num:
                    rule_num = parts[2]
            rule_oid = resolve_acl_rule_oid(group_id, rule_num)
            if rule_oid is None:
                log(
                    f"sai_remove_dash_acl_rule missing oid for {group_id}:{rule_num}"
                )
                asic.hset(
                    "SAI_OPI:LAST_ACL_REMOVE",
                    mapping={
                        "status": "missing_oid",
                        "group_id": group_id,
                        "rule_num": rule_num,
                    },
                )
            else:
                st = lib.lab_sai_remove_acl_rule(rule_oid)
                log(
                    f"sai_remove_dash_acl_rule group={group_id} rule={rule_num} "
                    f"oid=0x{rule_oid:x} status={st}"
                )
                asic.hset(
                    "SAI_OPI:LAST_ACL_REMOVE",
                    mapping={
                        "status": str(st),
                        "group_id": group_id,
                        "rule_num": rule_num,
                        "rule_oid": hex(rule_oid),
                    },
                )
                if group_id and rule_num:
                    asic.delete(acl_rule_oid_key(group_id, rule_num))

        else:
            log(f"unhandled/degraded op={kind}")


if __name__ == "__main__":
    main()
