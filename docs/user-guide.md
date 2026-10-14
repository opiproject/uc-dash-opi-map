<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
-->

# User guide — run the DASH → OPI lab

This is the operator cookbook. Architecture and SAI↔OPI field maps live in
[`README.md`](../README.md) and [`dash-opi-object-mapping.md`](dash-opi-object-mapping.md).
Hop-by-hop “what is live” is [`dash-api-flow-status.md`](dash-api-flow-status.md).

**Validated on NVIDIA BlueField-2:** gNMI Set of VNET, QoS, route group/entry,
ACL group/rule, and ENI → `libsai_opi_adapter.so` → CloudInfra `:50051` create
RPCs, then gNMI delete of ACL, route, ENI, groups, and VNET (the 4×4 Create/
Delete matrix). `run-sonic` writes `build/reports/run-sonic-latest.md`.
OVS log: [`example-output/run-sonic-nvidia-success.txt`](../example-output/run-sonic-nvidia-success.txt).
DOCA log (`DATAPATH=doca`, `49 passed, 0 failed`): [`example-output/run-sonic-nvidia-doca-success.txt`](../example-output/run-sonic-nvidia-doca-success.txt).

Intel / AMD / Marvell CloudInfra is the same RPC contract on a different SoC
daemon. This guide is the NVIDIA lab path.

---

## 1. What you are running

```text
Browser :8090 (dash-gui)  or  gnmic / run-sonic
        │  gNMI Set (DASH YANG/JSON)
sonic-gnmi :8080
        │  APPL_DB  DASH_*_TABLE
dashorch
        │  ASIC_DB  SAI_OPI:WORKQ
syncd  (dlopen libsai.so → libsai_opi_adapter.so)
        │  CloudInfra gRPC
DPU ARM  :50051   (vendor daemon: dpu/nvidia or dpu/intel)
```

Summit GUI: after `setup-sonic`, open **http://127.0.0.1:8090**. Buttons are
the same JSON_IETF payloads as `gnmic` (not grpcui protobuf forms). The page
shows APPL_DB and adapter `CreateVnic` / `DeleteVnic` lines.

| Port | Service | Used for DASH? |
| --- | --- | --- |
| **`:8090`** | `dash-gui` demo clicker | **Yes** — drives gNMI |
| **`:8080`** | `sonic-gnmi` | **Yes** — Set/Get |
| **`:50051`** | CloudInfra (`CreateVpc`, `CreateVnic`, `CreateRoute`, `CreateSecurityPolicy`, …) | **Yes** — adapter target |
| **`:50052`** | SNAP (`opi-nvidia-bridge`: inventory / NVMe / IPsec) | **No** — `SNAP channel unavailable` in syncd logs is OK |

Do not point `OPI_SERVER_ADDRESS` at `:50052`.

---

## 2. One-time machine setup

Run everything below **on the x86 host** (not on the DPU ARM), from a clone of
this repo.

### 2.1 Packages (Ubuntu 22.04 / jammy recommended)

The adapter `.so` links jammy `libprotobuf.so.23` / `libgrpc++`. The lab
`syncd` image is `ubuntu:22.04` for the same SONAMEs.

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git pkg-config docker.io \
  libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  python3 python3-pip
```

You need Docker on the host (compose stack) and SSH to the DPU (CloudInfra).

Optional clones if you do not want CMake FetchContent:

```bash
git clone --depth 1 https://github.com/opiproject/opi-api.git "$HOME/src/opi-api"
git clone --depth 1 https://github.com/googleapis/googleapis.git "$HOME/src/googleapis"
```

### 2.2 Site config (`config/lab.env`)

Do **not** edit IPs into `scripts/labctl.sh`. One file holds topology **and**
DPU prerequisites:

```bash
cp config/lab.env.example config/lab.env
# edit DPU_IP, HOST_IP, SSH_TARGET, DATAPATH, DPU_* prereqs
```

| Variable | Meaning | NVIDIA OOB lab example |
| --- | --- | --- |
| `DPU_IP` | DPU ARM address for SSH + CloudInfra | `172.22.3.2` |
| `HOST_IP` | Address **on this host** toward `DPU_IP` (OOB/LAN) | `172.22.1.2` on `ens10f0np0` |
| `SSH_TARGET` | SSH to the DPU | `root@172.22.3.2` |
| `VENDOR` | SoC daemon: `nvidia` (default) or `intel` | `nvidia` |
| `DATAPATH` | `ovs` (default), `doca`, or `auto` | `ovs` |
| `DPU_INSTALL_PACKAGES` | `setup-dpu` apt-gets Open vSwitch / build-essential | `1` |
| `DPU_UPLINK` / `DPU_HOST_REPRESENTORS` | netdevs to `ip link set up` | `p0` / `pf0hpf …` |
| `DPU_HUGEPAGES_2MB` | hugepages for DOCA Flow | `1024` |
| `DPU_DOCA_STUB` | `1` = JSON stub, no SDK | `0` |
| `DPU_SWITCHDEV` | `1` = attempt mlx5 eSwitch (disruptive) | `0` |
| `DPU_STEAL_REPS` | `1` = take representors off vendor `ovsbr1` | `0` |

`setup-sonic` writes `build/opi.env` from `DPU_IP` + `GRPC_PORT` / `SNAP_PORT`.
Do not keep a second topology in `config/opi.env`.

`HOST_IP` is **not** the RShim `tmfifo_net0` address. RShim is optional
sideband. CloudInfra and SSH use the OOB/LAN path. Preflight **warns** if
RShim is missing; it **fails** if `ping DPU_IP` fails.

Precedence: **CLI flags > `config/lab.env` > script defaults**.

---

## 3. Every labctl step

From the repo root:

```bash
./scripts/labctl.sh preflight     # (a) host PCIe/ping + read-only DPU inspect
./scripts/labctl.sh inspect-dpu   # SoC inventory only (OVS/DOCA/representors)
./scripts/labctl.sh setup-dpu     # (b) NVIDIA CloudInfra :50051 (+ SNAP :50052)
# ./scripts/labctl.sh setup-dpu --vendor intel
./scripts/labctl.sh setup-host    # (c) cmake-build libsai_opi_adapter.so
./scripts/labctl.sh run           # (d) optional C ABI smoke (skips gNMI)
./scripts/labctl.sh setup-sonic   # (e) gnmi + dashorch + syncd → DPU
./scripts/labctl.sh run-sonic     # (f) gNMI / SAI-Challenger E2E  ← control-plane gate
./scripts/labctl.sh run-iperf     # (g) two ENIs same VNET + iperf3 (DATAPATH=ovs)
```

One shot after `config/lab.env` is filled:

```bash
./scripts/labctl.sh all-sonic     # a → c, then e + f (skips C-only run)
```

Tear down:

```bash
./scripts/labctl.sh cleanup
# ./scripts/labctl.sh cleanup --keep-snap --keep-sonic
```

Cleanup stops CloudInfra, deletes `br-opi` only, and brings compose down. It
does **not** touch vendor `ovsbr1` / `p0` / `pf0hpf`.

---

### (a) `preflight` / `inspect-dpu`

```bash
./scripts/labctl.sh preflight      # host + DPU
./scripts/labctl.sh inspect-dpu    # DPU SoC only
```

Host checks:

| Check | Pass / fail |
| --- | --- |
| PCIe BlueField / DPU | **FAIL** if no card |
| RShim `tmfifo_net0` | **WARN** if missing (OK on OOB) |
| `HOST_IP` on some host iface | sanity |
| `ping DPU_IP` | **FAIL** if no L3 |

Then `inspect-dpu` SSHs to the SoC and **only reads** (no apt, no start, no kill):

| Check | Meaning |
| --- | --- |
| Linux / aarch64 | SoC OS |
| `ovs-vsctl` / `ovs-vswitchd` | already installed/running, or **WARN** (setup-dpu will install) |
| DOCA SDK / hugepages / sidecar socket | relevant when `DATAPATH=doca` |
| `p0` / `pf0hpf` / `pf0vf*` | representors present? |
| vendor `ovsbr1` | left alone unless `DPU_STEAL_REPS=1` |
| `:50051` / CloudInfra process | already up? setup-dpu will replace it |

Missing packages are warnings, not failures — that is what `setup-dpu` fixes. A table at the end lists gaps. Does not require `:50051` yet.

---

### (b) `setup-dpu`

Builds the NVIDIA daemon `dpu_cloudinfra_bridge` from [`dpu/nvidia/`](../dpu/nvidia/)
(ARM64, host Go — do **not** pass `--platform linux/arm64`), copies `config/lab.env`
and the binary to the DPU, then **installs prerequisites, prepares the packet
backend, and starts listeners**:

1. `--datapath ovs` (default): `apt-get install openvswitch-switch` if needed,
   start `ovs-vswitchd`, bring up `DPU_UPLINK` / representors. Fail if Open
   vSwitch cannot start. `br-opi` is still created by CloudInfra, not here.
   Vendor `ovsbr1` / `p0` / `pf0hpf` are left alone unless `DPU_STEAL_REPS=1`.
2. `--datapath doca`: hugepages, build/start `dpu_doca_flowd` (DOCA SDK unless
   `DPU_DOCA_STUB=1`) before CloudInfra. Do not start OVS for `br-opi`.
3. Then start CloudInfra on `:50051`, and SNAP on `:50052` unless `--no-snap`.

Intel Phase A: `./scripts/labctl.sh setup-dpu --vendor intel` (OVS only).

| Port | Binary | Role |
| --- | --- | --- |
| `:50051` | `dpu_cloudinfra_bridge` (NVIDIA) or `dpu_intel_bridge` | DASH CloudInfra + Telemetry |
| `:50052` | SNAP `opi-nvidia-bridge` | storage / inventory (optional) |

```bash
./scripts/labctl.sh setup-dpu
# CloudInfra only:
# ./scripts/labctl.sh setup-dpu --no-snap
```

Confirm from the host:

```bash
ss -lntp | grep 50051 || nc -vz "${DPU_IP:-172.22.3.2}" 50051
# optional:
sudo docker run --rm --network=host namely/grpc-cli \
  ls --json_input --json_output 172.22.3.2:50051 -l
```

Expect `opi_api.network.cloud.v1alpha1.CloudInfraService` and
`opi_api.telemetry.v1.TelemetryService`.

---

### (c) `setup-host`

```bash
./scripts/labctl.sh setup-host
ls -l build/libsai_opi_adapter.so build/test_sai_create_eni
```

This is the ELF `syncd` will `dlopen` as `libsai.so`.

---

### (d) `run` (optional C ABI)

Skips gNMI / dashorch. Useful to prove host → DPU gRPC before compose:

```bash
./scripts/labctl.sh run
# equivalent: ./build/test_sai_create_eni 172.22.3.2:50051
```

Success is `sai_create_eni` + `GetVnicMetrics`. SNAP `GetInventory` is probed
on `:50052`; a SNAP miss does not fail DASH unless `OPI_REQUIRE_SNAP=1`.

---

### (e) `setup-sonic`

Starts [`deploy/docker-compose.sonic-stack.yml`](../deploy/docker-compose.sonic-stack.yml)
**without** `mock-opi`, bind-mounts `build/libsai_opi_adapter.so`, and points
`OPI_SERVER_ADDRESS` at `DPU_IP:50051`.

```bash
./scripts/labctl.sh setup-sonic
```

This command also `ldd`s the adapter **inside** the `syncd` container and
fails if `libprotobuf` / `libgrpc++` are missing.

Expect:

| Container | Listen |
| --- | --- |
| `sonic-gnmi` | `:8080` |
| `dash-gui` | `:8090` (browser DASH gNMI clicker) |
| `redis-appl-db` | `:6379` |
| `dashorch` | APPL_DB watcher |
| `syncd` | BRPOP `SAI_OPI:WORKQ` |

Healthy `syncd` startup:

```text
syncd-lab: libsai.so -> /usr/lib/libsai_opi_adapter.so
syncd-lab: dlopen /usr/lib/libsai.so
sai-opi: CloudInfra connected target=172.22.3.2:50051
sai-opi: SNAP channel unavailable target=172.22.3.2:50052   # OK
syncd-lab: dash_opi_client_init(...) status=0
syncd-lab: BRPOP SAI_OPI:WORKQ on ASIC_DB
```

`status=0` then idle BRPOP (no traceback) is ready. Do **not** proceed if you
see `dlopen failed: libprotobuf.so.23` or a Python `TimeoutError` on `brpop`.

---

### (f) `run-sonic` — the NVIDIA networking gate

```bash
./scripts/labctl.sh run-sonic
```

What it does, in order:

1. Wait for gNMI `:8080` and Redis `:6379`
2. gNMI Set parent objects: VNET, QoS, route group, ACL group
3. gNMI Set children: ENI, outbound route, ACL rule
4. Check APPL_DB `DASH_*_TABLE` keys
5. Check dashorch enqueued `create_*`
6. Check syncd called real `sai_create_*` (not log-only `SAI op=`)
7. Score all **8 create** CloudInfra RPCs in syncd logs
8. TCP check CloudInfra `:50051`
9. Northbound pytest (APPL_DB only — 3 cases; runs before deletes)
10. gNMI **delete** the 4×4 children then parents:
    ACL rule → route → ENI → ACL group → route group → VNET
11. Score all **8 delete** CloudInfra RPCs
12. Write `build/reports/run-sonic-latest.md` and `.json`

Payloads: [`deploy/payloads/`](../deploy/payloads/).

**Green:** `results: N passed, 0 failed` and every row in the report’s 4×4
table is `PASS` (`CreateVpc` … `DeleteVpc`), including ENI `DeleteVnic` and
`DeleteInterface`. Blessed DOCA capture:
[`run-sonic-nvidia-doca-success.txt`](../example-output/run-sonic-nvidia-doca-success.txt)
(`49 passed, 0 failed`; `labctl cleanup` restored `ovsbr1` `p0` / `pf0hpf`).

gNMI `"time": "1970-01-01T00:00:00Z"` is the gnmic container clock, not a
failed Set. The pytest trio only checks northbound APPL_DB; the SAI/OPI hops
are the shell `PASS` lines and the report.

Optional extra check:

```bash
docker logs syncd 2>&1 | grep -E 'sai_create_|sai_remove_|CreateVpc|DeleteVnic'
cat build/reports/run-sonic-latest.md
```

Look for `status=0` on creates/remove and `grpc_status=0` on OPI RPCs.

On the DPU SoC, after `setup-dpu` + `run-sonic`, routes and ACLs are OpenFlow
on `br-opi` (not record-only) when `DATAPATH=ovs`:

```bash
ovs-ofctl -O OpenFlow13 dump-flows br-opi
```

| Table | What you should see for the lab payloads |
| --- | --- |
| 0 | ACL on the VNIC host representor `in_port` (permit tcp `tp_dst=80` → `goto_table:1`; deny → `drop`) |
| 1 | L3 `nw_dst=10.0.0.0/16` (or your route prefix) → set `tun_id` / drop |
| 2 | Existing VNIC overlay encap/decap |

If `goto_table` is missing, dump a single table: deny ≥400, route ≥300,
overlay 200.

DOCA backend (`DATAPATH=doca` in `lab.env`) is the same 16 CloudInfra RPCs
via `dpu_doca_flowd`, not mixed with `br-opi`. Control-plane green:
[`run-sonic-nvidia-doca-success.txt`](../example-output/run-sonic-nvidia-doca-success.txt).
`cleanup` restores stolen `ovsbr1` ports (`p0`, `pf0hpf`) when
`DPU_STEAL_REPS=1`. `-datapath=ovs` remains the default. `-datapath=auto`
uses the sidecar if `/var/run/opi-doca.sock` is up.

```bash
# on the DPU:
tail -f /var/log/dpu-doca-flowd.log
```

### (g) `run-iperf` — two ENIs in the same VNET + packets

`run-sonic` deletes the 4×4 objects. Packet proof needs two ENIs **left up**
in `Vnet1`, OVS same-VNI L2 (priority 220), and iperf3 on the host VFs.

Use **`DATAPATH=ovs`**. Program the second ENI from
[`dash_eni_b.json`](../deploy/payloads/dash_eni_b.json) (MAC `00:1a:2b:3c:4d:5f`)
plus ACL permit tcp/5201 ([`dash_acl_rule_iperf.json`](../deploy/payloads/dash_acl_rule_iperf.json)).

```bash
# config/lab.env: DATAPATH=ovs
# optional: HOST_VF_A=ens1f0v0 HOST_VF_B=ens1f0v1  (host SR-IOV VFs, not pf0vf*)
./scripts/labctl.sh setup-dpu
./scripts/labctl.sh setup-sonic
./scripts/labctl.sh run-iperf
```

`run-iperf` gNMI-sets VNET, both ENIs, `10.0.0.0/16`, and ACL (http + iperf3).
It does **not** delete. If `HOST_VF_A` / `HOST_VF_B` are set, it assigns
`10.0.0.1` / `10.0.0.2` and runs `iperf3 -c 10.0.0.1 -B 10.0.0.2`.

Manual (VFs unset):

```bash
sudo ip link set "$HOST_VF_A" address f4:93:9f:ef:c4:7e
sudo ip link set "$HOST_VF_B" address 00:1a:2b:3c:4d:5f
sudo ip addr add 10.0.0.1/16 dev "$HOST_VF_A"
sudo ip addr add 10.0.0.2/16 dev "$HOST_VF_B"
sudo ip link set "$HOST_VF_A" up
sudo ip link set "$HOST_VF_B" up
sudo iperf3 -s -B 10.0.0.1 -1 &
sudo iperf3 -c 10.0.0.1 -B 10.0.0.2 -t 10
```

On the DPU, `ovs-ofctl -O OpenFlow13 dump-flows br-opi` should show
`priority=220` `dl_dst=<peer MAC>` between the two representors, plus table 0/1
ACL/route. Pass: non-zero iperf3 throughput. Then `./scripts/labctl.sh cleanup`.

---

## 4. Software-only loop (no DPU)

Adapter + in-tree Python CloudInfra mock:

```bash
./scripts/labctl.sh setup-host
docker compose -f deploy/docker-compose.sonic-stack.yml --profile mock-opi up --build -d
./scripts/run_sai_challenger.sh --no-compose --profile mock-opi
```

Or C ABI against the mock:

```bash
python3 -m pip install -r tests/opi_server/requirements.txt
python3 tests/run_e2e.py --binary build/test_sai_create_eni
```

---

## 5. Logs to read (per hop)

```bash
docker logs sonic-gnmi 2>&1 | grep sonic-gnmi
docker exec redis-appl-db redis-cli -n 0 KEYS 'DASH_*'
docker logs dashorch 2>&1 | grep dashorch
docker logs syncd 2>&1 | grep -E 'syncd-lab:|sai-opi:'
docker exec syncd cat /var/log/sai-opi-adapter.log
```

| You see | Meaning |
| --- | --- |
| APPL_DB has `DASH_ENI_TABLE` but no `sai_create_eni` | dashorch/syncd not running or workq empty |
| `SAI op=create_eni` without `sai_create_eni` | log-only mode — adapter not loaded |
| `dlopen failed: libprotobuf.so.23` | rebuild `setup-sonic` (jammy syncd image) |
| `dash_opi_client_init status=12` | init failed (often failed dlopen, not OPI down) |
| `CreateVnic grpc_status=0` | CloudInfra accepted the ENI |
| `SNAP channel unavailable :50052` | expected for DASH |

---

## 6. Production SONiC image (not the compose lab)

After `setup-host`, bind a real `syncd` container:

```bash
sudo ./scripts/install_libsai_opi_adapter.sh \
  --lib build/libsai_opi_adapter.so \
  --opi-server 172.22.3.2:50051 \
  --opi-snap 172.22.3.2:50052

sudo ./scripts/enable-persistent-adapter.sh \
  --lib build/libsai_opi_adapter.so \
  --container syncd
```

Details: [README §6.3–6.4](../README.md#63-bind-sonic-syncd-to-the-adapter).

---

## 7. What is **not** in this guide yet

| Item | Status |
| --- | --- |
| ENI / VNET / outbound route / ACL **create + remove** on NVIDIA | **Done** — `run-sonic` 4×4 + `build/reports/run-sonic-latest.md` |
| Route / ACL **enforced on OVS** (`br-opi` tables 0–2) | **Done** — default `-datapath=ovs` |
| DOCA Flow / eSwitch control plane | **Done** — `DATAPATH=doca`; [`run-sonic-nvidia-doca-success.txt`](../example-output/run-sonic-nvidia-doca-success.txt) (`49 passed, 0 failed`) |
| `sai_get_eni_stats` / FlexCounter | Implemented in the `.so`; lab orch does not poll it |
| CA-to-PA, inbound routing, tunnel, meter, HA, VIP | Mapping only — see flow-status |
| Intel IPU CloudInfra (Phase A OVS) | **Parked** — complete NVIDIA first |
| iperf3 overlay packets | **Lab path** — `labctl run-iperf` (two ENIs in Vnet1, `DATAPATH=ovs`) |

---

## 8. Command cheat sheet

```bash
cp config/lab.env.example config/lab.env   # once
./scripts/labctl.sh inspect-dpu            # read-only SoC inventory
./scripts/labctl.sh all-sonic              # full NVIDIA E2E
./scripts/labctl.sh setup-dpu --vendor intel   # Intel Phase A CloudInfra
./scripts/labctl.sh run-sonic              # re-push gNMI (stack already up)
./scripts/labctl.sh run-iperf              # two ENIs same VNET + iperf3
./scripts/labctl.sh cleanup

# Summit demo GUI (after setup-sonic):
#   http://127.0.0.1:8090

docker logs syncd 2>&1 | tail -50
docker exec syncd ldd /usr/lib/libsai_opi_adapter.so | grep -E 'not found|protobuf|grpc'
```
