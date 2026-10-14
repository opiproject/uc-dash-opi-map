# DASH API flow — current status

End-to-end path this repository implements (and maps, but does not yet
implement). SNAP inventory / NVMe / IPsec is **not** on this path; see the
last section.

**Operator steps:** [`docs/user-guide.md`](user-guide.md).
NVIDIA BF2 control-plane green logs:
OVS [`example-output/run-sonic-nvidia-success.txt`](../example-output/run-sonic-nvidia-success.txt);
DOCA [`example-output/run-sonic-nvidia-doca-success.txt`](../example-output/run-sonic-nvidia-doca-success.txt)
(`DATAPATH=doca`, `49 passed, 0 failed`, `cleanup` restored `ovsbr1`).

```text
SAI-Challenger / gnmic
        │  gNMI Set (JSON_IETF, sonic-dash)
        ▼
sonic-gnmi :8080
        │  APPL_DB HSET  DASH_*_TABLE
        ▼
dashorch
        │  ASIC_DB work queue  SAI_OPI:WORKQ
        ▼
syncd  (dlopen libsai.so)
        │  SAI C ABI
        ▼
libsai_opi_adapter.so
        │  OPI gRPC  OPI_SERVER_ADDRESS :50051
        ▼
dpu_cloudinfra_bridge  (NVIDIA SoC; dpu/nvidia)
        │  ip link / OVS (ACL→L3→overlay) or DOCA Flow eSwitch (DATAPATH=doca)
        ▼
DPU datapath  (pf0hpf / pf0vf*, br-opi OpenFlow 1.3 — or DOCA control pipe)
```

Bypass used by `labctl.sh run`: `test_sai_create_eni` → adapter → CloudInfra.
That path skips SAI-Challenger, gNMI, dashorch, and the lab `syncd.py` work
queue.

## Status legend

| Tag | Meaning |
| --- | --- |
| **E2E** | Live through this hop and onto the next (or onto the DPU datapath) |
| **NB** | Northbound only: gNMI / APPL_DB / dashorch enqueue, then stops |
| **Map** | Specified in [`mapping/dash-opi.yaml`](../mapping/dash-opi.yaml); no adapter/DPU code |
| **—** | Not present at this hop |

## 1. Object flow (all DASH objects)

| Object | Challenger / gNMI | APPL_DB | dashorch | syncd SAI | Adapter | CloudInfra `:50051` | DPU SoC / datapath |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Lifecycle** `sai_api_initialize` | — | — | — | **E2E** | **E2E** open CloudInfra (+ SNAP inventory, not DASH) | TCP connect | — |
| **Lifecycle** `sai_api_query(SAI_API_DASH_ENI)` | — | — | — | **E2E** | **E2E** | — | — |
| **Lifecycle** `sai_api_query(SAI_API_DASH_VNET)` | — | — | — | **E2E** | **E2E** | — | — |
| **Lifecycle** `sai_api_query` other DASH APIs | — | — | — | **—** `NOT_IMPLEMENTED` | **—** | — | — |
| **VNET** | **E2E** `dash_vnet.json` | **E2E** `DASH_VNET_TABLE` | **E2E** `create_vnet` | **E2E** `sai_create_vnet` | **E2E** `CreateVpc` then `CreateSubnet` | **E2E** | **E2E** record Vpc/Subnet (VNI); overlay on ENI |
| **QoS / CPS** | **NB** `dash_qos.json` | **NB** `DASH_QOS_TABLE` | **NB** CPS looked up for ENI | packed as `SAI_ENI_ATTR_CPS` on create | **E2E** → `VnicSpec.max_cps` | stored on Vnic msg | **—** not enforced |
| **ENI create** | **E2E** `dash_eni.json` | **E2E** `DASH_ENI_TABLE` | **E2E** `create_eni` | **E2E** `sai_create_eni` | **E2E** `CreateInterface` then `CreateVnic` | **E2E** | **E2E** representor MAC/up, VXLAN/OVS or tc |
| **ENI remove** | **E2E** gNMI delete | **E2E** `DEL DASH_ENI_TABLE` | **E2E** `remove_eni` | **E2E** `sai_remove_eni` | **E2E** `DeleteVnic` then `DeleteInterface` | **E2E** | **E2E** drop overlay flows, release representor |
| **ENI set / get attr** | **—** | **—** | **—** | **—** | **—** | **—** | **—** |
| **ENI MAC map** (`eni_ether_address_map_entry`) | folded into ENI `mac_address` | ENI hash field | packed as `SAI_ENI_ATTR_VM_MAC` (vendor ext) | no map-entry SAI | MAC on HostIf + Vnic | MAC on representor | **E2E** `ip link set address` |
| **ENI stats** | **—** (FlexCounter / C ABI) | **—** | **—** in lab orch | **E2E** `sai_get_eni_stats` | **E2E** `GetVnicMetrics` | **E2E** TelemetryService | **E2E** sysfs rx/tx bytes/pkts + drops |
| **Outbound route group** | **E2E** `dash_route_group.json` | **E2E** `DASH_ROUTE_GROUP_TABLE` | **E2E** `create_route_group` | **E2E** `sai_create_outbound_routing_group` | **E2E** `CreateRouteTable` | **E2E** | **E2E** record RouteTable |
| **Outbound route entry** | **E2E** `dash_route.json` | **E2E** `DASH_ROUTE_TABLE` | **E2E** `create_route` | **E2E** `sai_create_outbound_routing_entry` | **E2E** `CreateRoute` | **E2E** | **E2E** OVS table 1 (`nw_dst`, VNI/drop) per VNIC `in_port` |
| **ACL group** | **E2E** `dash_acl_group.json` | **E2E** `DASH_ACL_GROUP_TABLE` | **E2E** `create_acl_group` | **E2E** `sai_create_dash_acl_group` | **E2E** `CreateSecurityPolicy` | **E2E** | **E2E** record SecurityPolicy |
| **ACL rule** | **E2E** `dash_acl_rule.json` | **E2E** `DASH_ACL_RULE_TABLE` | **E2E** `create_acl_rule` | **E2E** `sai_create_dash_acl_rule` | **E2E** `CreateSecurityRule` | **E2E** | **E2E** OVS table 0 (L3 + exact L4 port); deny=`drop`, allow=`goto` 1 |

**Live E2E today:** initialize + `sai_create_vnet` + outbound routing + ACL group/rule
+ `sai_create_eni` + `sai_get_eni_stats` + removes → `CreateVpc` / `CreateSubnet` /
`CreateRouteTable` / `CreateRoute` / `CreateSecurityPolicy` / `CreateSecurityRule` /
`CreateInterface` / `CreateVnic` / `GetVnicMetrics` / deletes. NVIDIA SoC programs
those routes/ACLs onto `br-opi` (OpenFlow 1.3 tables, or single-table fallback).
`labctl run-sonic` / gNMI delete walks remove through lab dashorch + syncd for
ACL, Route, ENI, and VNET (the 4×4). Report: `build/reports/run-sonic-latest.md`.

**Four-object NVIDIA contract is complete** for create/remove. Intel/AMD/Marvell each
ship a **similar** CloudInfra (same RPCs). See
[§8](#8-multi-vendor-feasibility--can-each-vendor-ship-cloudinfra-for-all-four).

## 2. Official DASH SAI modules vs this adapter

OCP [`saiextensions.h`](https://github.com/opencomputeproject/SAI/blob/master/experimental/saiextensions.h).
`sai_api_query` returns tables for `SAI_API_DASH_ENI`, `SAI_API_DASH_VNET`,
`SAI_API_DASH_OUTBOUND_ROUTING`, and `SAI_API_DASH_ACL`.

| `sai_api_t` | Typical SAI calls | Adapter | CloudInfra RPC (intended) |
| --- | --- | --- | --- |
| `SAI_API_DASH_ENI` | `create_eni`, `remove_eni`, `get_eni_stats` | **E2E** | `CreateInterface`, `CreateVnic`, `DeleteVnic`, `DeleteInterface`, `GetVnicMetrics` |
| | `set_eni_attribute`, `get_eni_attribute`, bulk create/remove | **—** | `UpdateVnic` |
| | `create_eni_ether_address_map_entry` (+ set/remove) | **—** (MAC stuffed into create) | `CreateMapping` |
| `SAI_API_DASH_VNET` | `create_vnet`, `remove_vnet` | **E2E** | `CreateVpc`, `CreateSubnet`, `DeleteSubnet`, `DeleteVpc` |
| | `set_vnet_attribute`, `get_vnet_attribute`, bulk | **—** | `UpdateVpc` / `UpdateSubnet` |
| `SAI_API_DASH_OUTBOUND_ROUTING` | `create_outbound_routing_group`, `create_outbound_routing_entry`, removes | **E2E** | `CreateRouteTable`, `CreateRoute`, `DeleteRoute`, `DeleteRouteTable` |
| `SAI_API_DASH_ACL` | `create_dash_acl_group`, `create_dash_acl_rule`, removes | **E2E** | `CreateSecurityPolicy`, `CreateSecurityRule`, `DeleteSecurityRule`, `DeleteSecurityPolicy` |
| `SAI_API_DASH_OUTBOUND_CA_TO_PA` | CA-to-PA / `Mapping` | **—** | `CreateMapping` |
| `SAI_API_DASH_INBOUND_ROUTING` | inbound routing entries | **—** | no first-class Cloud peer (often Mapping + Route) |
| `SAI_API_DASH_DIRECTION_LOOKUP` | VNI → direction | **—** | **Map** gap |
| `SAI_API_DASH_PA_VALIDATION` | PA allow-list | **—** | **Map** gap |
| `SAI_API_DASH_TUNNEL` | overlay tunnels | **—** | `CreateTunnel` |
| `SAI_API_DASH_METER` | meter policy / buckets | **—** | **Map** gap |
| `SAI_API_DASH_VIP` | VIP | **—** | **Map** gap |
| `SAI_API_DASH_HA` | HA scope | **—** | **Map** gap |
| `SAI_API_DASH_FLOW` | flow table / resimulation | **—** | **Map** gap |
| `SAI_API_DASH_APPLIANCE` | appliance / VIP underlay | **—** | **Map** gap |
| `SAI_API_DASH_OUTBOUND_PORT_MAP` | outbound port map | **—** | **Map** gap |
| `SAI_API_DASH_TRUSTED_VNI` | trusted VNI | **—** | **Map** gap |

## 3. `sai_create_eni` attributes

Parsed in `src/dash_opi/eni_attrs.h`. Unlisted official DASH ENI attrs are
ignored (create still succeeds).

| SAI attribute | gNMI / APPL_DB field | Packed to OPI? | DPU datapath |
| --- | --- | --- | --- |
| `SAI_ENI_ATTR_VNET_ID` | `vnet` | yes → `VnicSpec.subnet_name_ref` (name from ObjectStore) | unused (no VPC object) |
| `SAI_ENI_ATTR_CPS` | `DASH_QOS_TABLE.cps` | yes → `VnicSpec.max_cps` | **—** |
| `SAI_ENI_ATTR_VM_MAC` (vendor ext; DASH map key) | `mac_address` | yes → Interface + Vnic MAC | representor MAC |
| `SAI_ENI_ATTR_ADMIN_STATE` | `admin_state` | yes → `InterfaceSpec.admin_status` | `ip link set up/down` |
| `SAI_ENI_ATTR_VM_VNI` | VNET `vni` (lab copies VNET VNI) | yes → `VnicSpec.fabric_encap` VXLAN | OVS/tc VXLAN match |
| `SAI_ENI_ATTR_DASH_ENI_MODE` | `mode` | parsed; VM assumed | representor attach |
| `SAI_ENI_ATTR_PPS` | QoS `bw` | **—** | **—** |
| `SAI_ENI_ATTR_FLOWS` | QoS `flows` | **—** | **—** |
| `SAI_ENI_ATTR_HA_SCOPE_ID` | **—** | **—** | **—** |
| `SAI_ENI_ATTR_VM_UNDERLAY_DIP` | `underlay_ip` | **—** | **—** |
| ACL stage group ids, outbound routing group id, DSCP, PL_* | **—** in lab gNMI | **—** | **—** |

## 4. `sai_get_eni_stats` counters

| SAI counter | `GetVnicMetrics` | DPU source |
| --- | --- | --- |
| `SAI_ENI_STAT_RX_BYTES` / `RX_PACKETS` | **E2E** | representor sysfs |
| `SAI_ENI_STAT_TX_BYTES` / `TX_PACKETS` | **E2E** | representor sysfs |
| `SAI_ENI_STAT_FORWARDING_DROP_PACKETS` | **E2E** | mapped from drops |
| `SAI_ENI_STAT_POLICY_DROP_PACKETS` | **E2E** | still 0 (OVS ACL drops not mapped to this counter) |
| `SAI_ENI_STAT_OTHER_DROP_PACKETS` | **E2E** | `rx_drops + tx_drops` |
| `SAI_ENI_STAT_TOTAL_DROP_PACKETS` | **E2E** | sum of drop fields |
| All other `SAI_ENI_STAT_*` | filled **0** | — |

## 5. CloudInfra RPCs on `:50051`

Official `CloudInfraService` (opi-api) is much larger. This DPU daemon and the
in-tree mock only serve the RPCs below. The adapter **calls** only these.

| RPC | Adapter | DPU bridge | Datapath |
| --- | --- | --- | --- |
| `CreateInterface` | **E2E** | **E2E** | bind `pf0hpf`/`pf0vf*`, MAC, admin |
| `CreateVnic` | **E2E** | **E2E** | overlay (table 2) + re-install routes/ACLs on that `in_port` |
| `DeleteVnic` | **E2E** (`sai_remove_eni`) | **E2E** | drop overlay; refresh remaining route/ACL `in_port`s |
| `DeleteInterface` | **E2E** (`sai_remove_eni`; also Vnic-create rollback) | **E2E** | forget representor |
| `CreateVpc` | **E2E** (`sai_create_vnet`) | **E2E** | record tenant VPC + VNI |
| `CreateSubnet` | **E2E** (`sai_create_vnet`) | **E2E** | default subnet under Vpc |
| `DeleteSubnet` | **E2E** (`sai_remove_vnet`) | **E2E** | forget subnet |
| `DeleteVpc` | **E2E** (`sai_remove_vnet`) | **E2E** | forget VPC |
| `CreateRouteTable` | **E2E** (`sai_create_outbound_routing_group`) | **E2E** | record RouteTable |
| `CreateRoute` | **E2E** (`sai_create_outbound_routing_entry`) | **E2E** | OVS table 1: `ip,in_port=host,nw_dst=prefix` (VPC→VNI, DROP→drop) |
| `DeleteRoute` | **E2E** (`sai_remove_outbound_routing_entry`) | **E2E** | delete route cookie; forget Route |
| `DeleteRouteTable` | **E2E** (`sai_remove_outbound_routing_group`) | **E2E** | forget RouteTable |
| `CreateSecurityPolicy` | **E2E** (`sai_create_dash_acl_group`) | **E2E** | record SecurityPolicy |
| `CreateSecurityRule` | **E2E** (`sai_create_dash_acl_rule`) | **E2E** | OVS table 0 on each VNIC `in_port` (L3 + exact L4); permit=`goto_table:1` |
| `DeleteSecurityRule` | **E2E** (`sai_remove_dash_acl_rule`) | **E2E** | delete ACL cookie; forget SecurityRule |
| `DeleteSecurityPolicy` | **E2E** (`sai_remove_dash_acl_group`) | **E2E** | forget SecurityPolicy |
| `GetVnicMetrics` (`TelemetryService`) | **E2E** | **E2E** | sysfs counters |
| `UpdateVnic` / `GetVnic` / `UpdateVpc` / `GetVpc` | **—** | **—** | **—** |
| `CreateMapping` / `CreateTunnel` | **—** | **—** | **—** |

**ASIC note:** the SoC programs Linux (OVS/`br-opi` OpenFlow 1.3 tables 0–2,
or tc-flower deny) on NVIDIA host representors by default. **DOCA Flow /
eSwitch** (`DATAPATH=doca`) is a second backend behind the **same**
CloudInfra RPCs via `dpu_doca_flowd` (CONTROL pipe: ACL drop, L3 VXLAN
encap, overlay encap/decap) — not mixed with `br-opi`, and not linked into
`libsai_opi_adapter.so`. Control-plane 4×4 is E2E on DOCA
([`run-sonic-nvidia-doca-success.txt`](../example-output/run-sonic-nvidia-doca-success.txt)).
East-west packets on one BF2: `labctl run-iperf` with `DATAPATH=ovs` (two ENIs
in `Vnet1`). Treat “DPU ASIC” as “SoC datapath.”

OVS pipeline (per VNIC host representor `in_port`, so inbound VXLAN is not
classified as outbound ACL/L3):

| Table | Role | Miss |
| --- | --- | --- |
| 0 | ACL: deny → `drop`; allow → `goto_table:1` | `goto_table:1` (default-allow) |
| 1 | L3: `ROUTE_CLASS_VPC` → set `tun_id` + encap; unspecified → `drop` | `goto_table:2` |
| 2 | Overlay encap/decap (existing VNIC VXLAN) | — |

If OpenFlow 1.3 / `goto_table` is missing: single table (deny ≥400, route
≥300, overlay 200); permit ACL is default-allow (not installed). Exact L4
ports only (`80–80`); ranges are omitted from the match.

DOCA (`-datapath=doca`) uses one control pipe on the eSwitch instead of
those three OVS tables. Same objects, same RPC names, no `br-opi`.

## 6. gNMI paths this lab accepts

[`deploy/gnmi-lab/server.py`](../deploy/gnmi-lab/server.py) `Set`:

| Path fragment | APPL_DB | Then |
| --- | --- | --- |
| `DASH_VNET` | `DASH_VNET_TABLE` | **E2E** `create_vnet` / gNMI delete → `remove_vnet` |
| `DASH_QOS` | `DASH_QOS_TABLE` | CPS for ENI |
| `DASH_ENI` | `DASH_ENI_TABLE` | **E2E** create ENI; gNMI delete → **E2E** `remove_eni` |
| `DASH_ROUTE_GROUP` | `DASH_ROUTE_GROUP_TABLE` | **E2E** create/remove group |
| `DASH_ROUTE` | `DASH_ROUTE_TABLE` | **E2E** create/remove entry |
| `DASH_ACL_GROUP` | `DASH_ACL_GROUP_TABLE` | **E2E** create/remove group |
| `DASH_ACL_RULE` | `DASH_ACL_RULE_TABLE` | **E2E** create/remove rule |
| Any other DASH YANG (appliance, tunnel, mapping, meter, HA, …) | rejected | **—** |

## 7. Not this pipeline (SNAP `:50052`)

Do not mix these into DASH gNMI / dashorch.

| Call | When | Target |
| --- | --- | --- |
| `InventoryService.GetInventory` | `sai_api_initialize` | SNAP |
| `sai_opi_create_nvme_controller` | C test helper only | SNAP `CreateNvmeController` |
| `sai_opi_enable_ipsec` | C test helper only | SNAP `IPsecInitiate` / `IPsecRekey` |

## 8. Multi-vendor feasibility — can each vendor ship CloudInfra for all four?

**Clarified gate:** If we enhance NVIDIA CloudInfra (`dpu/nvidia`,
`dpu_cloudinfra_bridge`)
so it implements **ENI + VNET + outbound routing + ACL**, can Intel, AMD
(Pensando), and Marvell each ship a **similar CloudInfra daemon** that covers
the same four objects?

### Short answer

**Yes — by design.** The host keeps one `libsai_opi_adapter.so`. Each vendor
runs its **own** SoC process that implements the **same**
`CloudInfraService` (+ `TelemetryService`) RPCs for those four objects and
programs **its** datapath/SDK behind that facade.

```text
                    libsai_opi_adapter.so   (one host .so)
                              │
              OPI CloudInfraService :50051  (same protos / RPC names)
           ┌──────────────────┼──────────────────┬──────────────────┐
           ▼                  ▼                  ▼                  ▼
   NVIDIA CloudInfra   Intel CloudInfra   AMD CloudInfra   Marvell CloudInfra
   (this repo today)   (new SoC daemon)   (new SoC daemon) (new SoC daemon)
           │                  │                  │                  │
      BF representors      IPDK / P4         Pensando DSC      OCTEON / vendor
      OVS or DOCA Flow     pipelines         native tables     SDK / Linux
```

SNAP storage bridges (`opi-nvidia-bridge`, `opi-intel-bridge`) stay on
`:50052` and do **not** replace DASH CloudInfra.

### Per-vendor reality check (all four objects)

| Vendor | Can a CloudInfra cover ENI + VNET + Route + ACL? | How (typical) | Blockers today |
| --- | --- | --- | --- |
| **NVIDIA** | **Yes** — lab path already has ENI; VNET/route/ACL extend the same daemon | Representors + OVS tables 0–2 (default); optional DOCA Flow sidecar (`-datapath=doca`) behind the same RPCs | Networking not in public `opi-nvidia-bridge` (storage); this repo’s daemon is the DASH path |
| **Intel** | **Yes** — same RPCs; new/extended SoC daemon (not a second host `.so`) | IPDK / P4-DPDK / IPU pipeline maps Vnic/Vpc/Route/Security* | Public `opi-intel-bridge` is storage-only; need IPU SDK access + engineering |
| **AMD Pensando** | **Yes** — DSC already centers on VPC / ENI-like / route / policy | Bridge Pensando config model → OPI `Vpc`/`Vnic`/`Route`/`SecurityPolicy` | No public OPI DASH CloudInfra in this stack; needs vendor API access or partnership |
| **Marvell** | **Yes in principle** — same contract if the SoC can program overlay + ACL | OCTEON (or other) SDK / Linux datapath behind CloudInfra | Least public OPI DASH reference; depends on SDK docs and card bring-up |

So: enhancing NVIDIA CloudInfra for the four objects does **not** lock you to
NVIDIA. It **defines the contract** the other three must implement. You do
**not** fork the host adapter per vendor.

### What “similar CloudInfra” means (must match)

Each vendor daemon must accept at least:

| Object | CloudInfra RPCs (minimum create/delete) |
| --- | --- |
| **ENI** | `CreateInterface` / `DeleteInterface`, `CreateVnic` / `DeleteVnic`, `GetVnicMetrics` |
| **VNET** | `CreateVpc` / `DeleteVpc`, `CreateSubnet` / `DeleteSubnet` |
| **Routing** | `CreateRouteTable` / `DeleteRouteTable`, `CreateRoute` / `DeleteRoute` |
| **ACL** | `CreateSecurityPolicy` / `DeleteSecurityPolicy`, `CreateSecurityRule` / `DeleteSecurityRule` |

Fidelity of the **datapath** may differ (software OVS vs ASIC offload). The
**gRPC surface** should not.

### What is *not* required

- One binary that runs on every vendor SoC
- Waiting for upstream `opi-*-bridge` storage images to grow networking
- Putting DOCA / IPDK / Pensando / Marvell SDKs into `libsai_opi_adapter.so`

### Decision for this repository

| Question | Answer |
| --- | --- |
| Enhance NVIDIA CloudInfra with VNET + routing + ACL? | **Yes** — establishes the four-object contract on real hardware. |
| Can Intel / AMD / Marvell get a **similar** CloudInfra for all four later? | **Yes** — separate SoC daemons, same OPI RPCs; host adapter unchanged. |
| Available as drop-in public images today for Intel/AMD/Marvell DASH? | **No** — those are follow-on SoC projects (or vendor contributions), not blockers for the NVIDIA + adapter work. |
