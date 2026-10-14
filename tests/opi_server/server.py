"""
Mock OPI CloudInfraService + TelemetryService for DASH SAI → gRPC tests.

Uses the same proto subset as the C++ wrapper (package
opi_api.network.cloud.v1alpha1 and opi_api.telemetry.v1). Prints
CreateInterface / CreateVnic / DeleteVnic fields packed from SAI_ENI_ATTR_*
and GetVnicMetrics lookups from sai_get_eni_stats.

    pip install -r tests/opi_server/requirements.txt
    python tests/opi_server/server.py --port 50051
"""

from __future__ import annotations

import argparse
import sys
from concurrent import futures
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_PROTO_ROOT = _REPO_ROOT / "proto"
_GEN_DIR = Path(__file__).resolve().parent / "_gen"


def _ensure_stubs() -> None:
    _GEN_DIR.mkdir(parents=True, exist_ok=True)
    marker = _GEN_DIR / ".stamp"
    protos = [
        _PROTO_ROOT / "network" / "opinetcommon" / "networktypes.proto",
        _PROTO_ROOT / "network" / "cloud" / "vnic.proto",
        _PROTO_ROOT / "network" / "cloud" / "interface.proto",
        _PROTO_ROOT / "network" / "cloud" / "vpc.proto",
        _PROTO_ROOT / "network" / "cloud" / "subnet.proto",
        _PROTO_ROOT / "network" / "cloud" / "route.proto",
        _PROTO_ROOT / "network" / "cloud" / "networkpolicy.proto",
        _PROTO_ROOT / "network" / "cloud" / "cloudrpc.proto",
        _PROTO_ROOT / "telemetry" / "v1" / "telemetry.proto",
    ]
    newest = max(p.stat().st_mtime for p in protos)
    if marker.exists() and marker.stat().st_mtime >= newest:
        _add_pkg_inits()
        sys.path.insert(0, str(_GEN_DIR))
        return

    import grpc_tools
    from grpc_tools import protoc

    well_known = Path(grpc_tools.__file__).resolve().parent / "_proto"
    args = [
        "protoc",
        f"-I{_PROTO_ROOT.as_posix()}",
        f"-I{well_known.as_posix()}",
        f"--python_out={_GEN_DIR.as_posix()}",
        f"--grpc_python_out={_GEN_DIR.as_posix()}",
        *[str(p.relative_to(_PROTO_ROOT)).replace("\\", "/") for p in protos],
    ]
    # grpc_tools.protoc expects import paths relative to -I
    rc = protoc.main(args)
    if rc != 0:
        raise SystemExit(f"protoc failed with {rc}: {args}")
    _add_pkg_inits()
    marker.write_text("ok\n", encoding="utf-8")
    sys.path.insert(0, str(_GEN_DIR))


def _add_pkg_inits() -> None:
    for d in (
        _GEN_DIR,
        _GEN_DIR / "network",
        _GEN_DIR / "network" / "cloud",
        _GEN_DIR / "network" / "opinetcommon",
        _GEN_DIR / "telemetry",
        _GEN_DIR / "telemetry" / "v1",
    ):
        d.mkdir(parents=True, exist_ok=True)
        init = d / "__init__.py"
        if not init.exists():
            init.write_text("", encoding="utf-8")


def _fmt_mac(raw: bytes) -> str:
    if not raw:
        return "(empty)"
    return ":".join(f"{b:02x}" for b in raw)


def _if_status(value: int) -> str:
    return {0: "UNSPECIFIED", 1: "UP", 2: "DOWN"}.get(value, str(value))


def serve(port: int) -> None:
    _ensure_stubs()

    import grpc
    from network.cloud import (
        cloudrpc_pb2,
        cloudrpc_pb2_grpc,
        interface_pb2,
        networkpolicy_pb2,
        route_pb2,
        subnet_pb2,
        vnic_pb2,
        vpc_pb2,
    )
    from telemetry.v1 import telemetry_pb2, telemetry_pb2_grpc

    created_vnics: set[str] = set()
    created_vpcs: set[str] = set()
    created_subnets: dict[str, str] = {}  # subnet -> vpc
    created_route_tables: set[str] = set()
    created_routes: dict[str, str] = {}  # route -> table
    created_policies: set[str] = set()
    created_rules: dict[str, str] = {}  # rule -> policy

    class CloudInfra(cloudrpc_pb2_grpc.CloudInfraServiceServicer):
        def CreateInterface(self, request, context):
            iface = request.interface
            spec = iface.spec
            host = spec.host_if_spec
            print("=== CreateInterface ===", flush=True)
            print(f"  parent:         {request.parent or '(empty)'}", flush=True)
            print(f"  interface_id:   {request.interface_id}", flush=True)
            print(f"  name:           {iface.name}", flush=True)
            print(f"  admin_status:   {_if_status(spec.admin_status)}", flush=True)
            print(f"  host_if.vf:     {host.vf}", flush=True)
            print(f"  host_if.mac:    {_fmt_mac(host.mac_address)}", flush=True)
            print(flush=True)
            out = interface_pb2.Interface()
            out.name = iface.name or f"interfaces/{request.interface_id}"
            out.spec.CopyFrom(spec)
            return out

        def CreateVnic(self, request, context):
            vnic = request.vnic
            spec = vnic.spec
            encap = spec.fabric_encap
            print("=== CreateVnic ===", flush=True)
            print(f"  parent:              {request.parent or '(empty)'}", flush=True)
            print(f"  vnic_id:             {request.vnic_id}", flush=True)
            print(f"  name:                {vnic.name}", flush=True)
            print(f"  subnet_name_ref:     {spec.subnet_name_ref}", flush=True)
            print(f"  max_cps:             {spec.max_cps}", flush=True)
            print(f"  mac_address:         {_fmt_mac(spec.mac_address)}", flush=True)
            print(f"  host_if_name_ref:    {spec.host_if_name_ref}", flush=True)
            print(f"  fabric_encap.type:   {encap.type}", flush=True)
            print(f"  fabric_encap.vnid:   {encap.value.vnid}", flush=True)
            print(flush=True)
            out = vnic_pb2.Vnic()
            out.name = vnic.name or f"vnic/{request.vnic_id}"
            out.spec.CopyFrom(spec)
            created_vnics.add(out.name)
            return out

        def DeleteVnic(self, request, context):
            print("=== DeleteVnic ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_vnics:
                created_vnics.discard(request.name)
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

        def DeleteInterface(self, request, context):
            print("=== DeleteInterface ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            return Empty()

        def CreateVpc(self, request, context):
            vpc = request.vpc
            spec = vpc.spec
            encap = spec.fabric_encap
            print("=== CreateVpc ===", flush=True)
            print(f"  parent:              {request.parent or '(empty)'}", flush=True)
            print(f"  vpc_id:              {request.vpc_id}", flush=True)
            print(f"  name:                {vpc.name}", flush=True)
            print(f"  type:                {spec.type}", flush=True)
            print(f"  fabric_encap.type:   {encap.type}", flush=True)
            print(f"  fabric_encap.vnid:   {encap.value.vnid}", flush=True)
            print(flush=True)
            out = vpc_pb2.Vpc()
            out.name = vpc.name or f"vpcs/{request.vpc_id}"
            out.spec.CopyFrom(spec)
            created_vpcs.add(out.name)
            return out

        def DeleteVpc(self, request, context):
            print("=== DeleteVpc ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_vpcs:
                created_vpcs.discard(request.name)
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

        def CreateSubnet(self, request, context):
            subnet = request.subnet
            spec = subnet.spec
            print("=== CreateSubnet ===", flush=True)
            print(f"  parent:              {request.parent or '(empty)'}", flush=True)
            print(f"  subnet_id:           {request.subnet_id}", flush=True)
            print(f"  name:                {subnet.name}", flush=True)
            print(f"  vpc_name_ref:        {spec.vpc_name_ref}", flush=True)
            print(flush=True)
            if spec.vpc_name_ref and spec.vpc_name_ref not in created_vpcs:
                context.set_code(grpc.StatusCode.FAILED_PRECONDITION)
                context.set_details(f"vpc {spec.vpc_name_ref} not created")
                return subnet_pb2.Subnet()
            out = subnet_pb2.Subnet()
            out.name = subnet.name or f"subnets/{request.subnet_id}"
            out.spec.CopyFrom(spec)
            created_subnets[out.name] = spec.vpc_name_ref
            return out

        def DeleteSubnet(self, request, context):
            print("=== DeleteSubnet ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_subnets:
                del created_subnets[request.name]
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

        def CreateRouteTable(self, request, context):
            table = request.routetable
            spec = table.spec
            print("=== CreateRouteTable ===", flush=True)
            print(f"  parent:              {request.parent or '(empty)'}", flush=True)
            print(f"  routetable_id:       {request.routetable_id}", flush=True)
            print(f"  name:                {table.name}", flush=True)
            print(f"  af:                  {spec.af}", flush=True)
            print(flush=True)
            out = route_pb2.RouteTable()
            out.name = table.name or f"routetables/{request.routetable_id}"
            out.spec.CopyFrom(spec)
            created_route_tables.add(out.name)
            return out

        def DeleteRouteTable(self, request, context):
            print("=== DeleteRouteTable ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_route_tables:
                created_route_tables.discard(request.name)
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

        def CreateRoute(self, request, context):
            route = request.route
            spec = route.spec
            attrs = spec.attrs
            prefix = attrs.prefix
            addr = prefix.addr
            print("=== CreateRoute ===", flush=True)
            print(f"  parent:                 {request.parent or '(empty)'}", flush=True)
            print(f"  route_id:               {request.route_id}", flush=True)
            print(f"  name:                   {route.name}", flush=True)
            print(f"  route_table_name_ref:   {spec.route_table_name_ref}", flush=True)
            print(f"  route_class:            {attrs.route_class}", flush=True)
            print(f"  prefix.af:              {addr.af}", flush=True)
            print(f"  prefix.v4_addr:         {addr.v4_addr}", flush=True)
            print(f"  prefix.len:             {prefix.len}", flush=True)
            if attrs.HasField("vpc_nh_info"):
                print(
                    f"  vpc_nh_info.vpc_name_ref: {attrs.vpc_nh_info.vpc_name_ref}",
                    flush=True,
                )
            if attrs.HasField("eval_mapping"):
                print(f"  eval_mapping:           {attrs.eval_mapping}", flush=True)
            print(flush=True)
            if (
                spec.route_table_name_ref
                and spec.route_table_name_ref not in created_route_tables
            ):
                context.set_code(grpc.StatusCode.FAILED_PRECONDITION)
                context.set_details(
                    f"route table {spec.route_table_name_ref} not created"
                )
                return route_pb2.Route()
            out = route_pb2.Route()
            out.name = route.name or f"routes/{request.route_id}"
            out.spec.CopyFrom(spec)
            created_routes[out.name] = spec.route_table_name_ref
            return out

        def DeleteRoute(self, request, context):
            print("=== DeleteRoute ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_routes:
                del created_routes[request.name]
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

        def CreateSecurityPolicy(self, request, context):
            policy = request.securitypolicy
            spec = policy.spec
            print("=== CreateSecurityPolicy ===", flush=True)
            print(f"  parent:              {request.parent or '(empty)'}", flush=True)
            print(f"  securitypolicy_id:   {request.securitypolicy_id}", flush=True)
            print(f"  name:                {policy.name}", flush=True)
            print(f"  type:                {spec.type}", flush=True)
            print(f"  address_family:      {spec.address_family}", flush=True)
            print(f"  stateless:           {spec.stateless}", flush=True)
            print(flush=True)
            out = networkpolicy_pb2.SecurityPolicy()
            out.name = policy.name or f"securitypolicys/{request.securitypolicy_id}"
            out.spec.CopyFrom(spec)
            created_policies.add(out.name)
            return out

        def DeleteSecurityPolicy(self, request, context):
            print("=== DeleteSecurityPolicy ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_policies:
                created_policies.discard(request.name)
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

        def CreateSecurityRule(self, request, context):
            rule = request.securityrule
            spec = rule.spec
            attrs = spec.attrs
            print("=== CreateSecurityRule ===", flush=True)
            print(f"  parent:                      {request.parent or '(empty)'}", flush=True)
            print(f"  securityrule_id:             {request.securityrule_id}", flush=True)
            print(f"  name:                        {rule.name}", flush=True)
            print(
                f"  security_policy_name_ref:    {spec.security_policy_name_ref}",
                flush=True,
            )
            print(f"  priority:                    {attrs.priority}", flush=True)
            print(f"  action:                      {attrs.action}", flush=True)
            print(f"  terminating:                 {attrs.terminating}", flush=True)
            if attrs.match.HasField("l3_match"):
                l3 = attrs.match.l3_match
                print(f"  proto_num:                   {l3.proto_num}", flush=True)
                if l3.HasField("src_prefix"):
                    print(
                        f"  src_prefix:                  {l3.src_prefix.addr.v4_addr}/{l3.src_prefix.len}",
                        flush=True,
                    )
                if l3.HasField("dst_prefix"):
                    print(
                        f"  dst_prefix:                  {l3.dst_prefix.addr.v4_addr}/{l3.dst_prefix.len}",
                        flush=True,
                    )
            print(flush=True)
            if (
                spec.security_policy_name_ref
                and spec.security_policy_name_ref not in created_policies
            ):
                context.set_code(grpc.StatusCode.FAILED_PRECONDITION)
                context.set_details(
                    f"security policy {spec.security_policy_name_ref} not created"
                )
                return networkpolicy_pb2.SecurityRule()
            out = networkpolicy_pb2.SecurityRule()
            out.name = rule.name or f"securityrules/{request.securityrule_id}"
            out.spec.CopyFrom(spec)
            created_rules[out.name] = spec.security_policy_name_ref
            return out

        def DeleteSecurityRule(self, request, context):
            print("=== DeleteSecurityRule ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            print(f"  allow_missing:  {request.allow_missing}", flush=True)
            print(flush=True)
            from google.protobuf.empty_pb2 import Empty

            if request.name in created_rules:
                del created_rules[request.name]
            elif not request.allow_missing:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
            return Empty()

    class Telemetry(telemetry_pb2_grpc.TelemetryServiceServicer):
        def GetVnicMetrics(self, request, context):
            print("=== GetVnicMetrics ===", flush=True)
            print(f"  name:           {request.name}", flush=True)
            if not request.name:
                context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
                context.set_details("name is required")
                return telemetry_pb2.VnicMetrics()
            if request.name not in created_vnics:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details(f"{request.name} not found")
                return telemetry_pb2.VnicMetrics()
            out = telemetry_pb2.VnicMetrics()
            # Deterministic hardware counters for sai_get_eni_stats tests.
            out.rx_bytes = 1000
            out.rx_packets = 10
            out.tx_bytes = 2000
            out.tx_packets = 20
            out.rx_drops = 1
            out.tx_drops = 2
            out.policy_drops = 3
            out.forwarding_drops = 4
            print(
                f"  rx_bytes={out.rx_bytes} rx_packets={out.rx_packets} "
                f"tx_bytes={out.tx_bytes} tx_packets={out.tx_packets} "
                f"rx_drops={out.rx_drops} tx_drops={out.tx_drops} "
                f"policy_drops={out.policy_drops} "
                f"forwarding_drops={out.forwarding_drops}",
                flush=True,
            )
            print(flush=True)
            return out

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    cloudrpc_pb2_grpc.add_CloudInfraServiceServicer_to_server(CloudInfra(), server)
    telemetry_pb2_grpc.add_TelemetryServiceServicer_to_server(Telemetry(), server)
    try:
        from grpc_reflection.v1alpha import reflection

        reflection.enable_server_reflection(
            (
                cloudrpc_pb2.DESCRIPTOR.services_by_name["CloudInfraService"].full_name,
                telemetry_pb2.DESCRIPTOR.services_by_name["TelemetryService"].full_name,
                reflection.SERVICE_NAME,
            ),
            server,
        )
    except ImportError:
        print(
            "grpcio-reflection not installed; grpc_cli ls / grpcurl list will not work",
            flush=True,
        )
    listen = f"[::]:{port}"
    server.add_insecure_port(listen)
    server.start()
    print(
        f"OPI mock CloudInfraService + TelemetryService listening on {listen}",
        flush=True,
    )
    server.wait_for_termination()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Mock OPI CloudInfraService + TelemetryService")
    parser.add_argument("--port", type=int, default=50051)
    serve(parser.parse_args().port)


if __name__ == "__main__":
    main()
