<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
-->

# DOCA Flow / eSwitch sidecar

CloudInfra RPCs stay on `dpu_cloudinfra_bridge`. This helper owns **DOCA Flow
switch mode** (eSwitch pipes) so the Go daemon and the host adapter do not
link `libdoca` or run DPDK EAL.

The host adapter (`libsai_opi_adapter.so`) programs this pipeline **only**
through CloudInfra gRPC. Set `DATAPATH=doca` in `config/lab.env`.

| Binary | Role |
| --- | --- |
| `dpu_cloudinfra_bridge -datapath ovs` | Default. `br-opi` tables 0–2. No DOCA. |
| `dpu_cloudinfra_bridge -datapath doca` | Same 16 RPCs; Interface/Vnic/Route/SecurityRule go here |
| `dpu_cloudinfra_bridge -datapath auto` | Sidecar if `/var/run/opi-doca.sock` answers, else OVS |

Do **not** mix: one daemon, one backend. Vpc/Subnet/RouteTable/SecurityPolicy
are still in-memory on both.

## eSwitch pipeline (`make DOCA=1`)

Root **CONTROL** pipe (same idea as OVS tables 0–2):

| Kind | Match | Action |
| --- | --- | --- |
| ACL deny | IPv4 5-tuple / prefixes | `FWD_DROP` |
| ACL permit | — | default-allow (no entry) |
| L3 route | IPv4 dest prefix | VXLAN encap + VNI → uplink, or drop |
| Overlay decap | VXLAN VNI (+ dest MAC) | decap → host representor |
| Overlay encap | representor / src MAC | VXLAN encap → uplink |

`setup-dpu` installs hugepages, builds this sidecar, and starts the socket
before CloudInfra.

## Bring-up

Prefer from the host (uses `config/lab.env`):

```bash
# in config/lab.env: DATAPATH=doca
./scripts/labctl.sh setup-dpu
```

On the DPU by hand:

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs -o pagesize=2M nodev /mnt/huge

cd dpu/nvidia/doca
make DOCA=1          # or `make` / DPU_DOCA_STUB=1 for protocol-only
sudo ./dpu_doca_flowd -s /var/run/opi-doca.sock
sudo ./dpu/nvidia/dpu_cloudinfra_bridge -datapath doca
```

`make DOCA=1` targets **DOCA 2.7+**. The sidecar opens the mlx5 PF, probes it
with `doca_dpdk_port_probe` (`dv_flow_en=2` + switch HWS args), then starts
one switch-manager port (`set_dev` / `set_port_id`). If link/init fails, leave
`-datapath ovs` or set `DPU_DOCA_STUB=1`.

Control-plane 4×4 on BF2 (`DATAPATH=doca`):
[`example-output/run-sonic-nvidia-doca-success.txt`](../../../example-output/run-sonic-nvidia-doca-success.txt)
(`49 passed, 0 failed`). `labctl cleanup` restores `ovsbr1` if `DPU_STEAL_REPS=1`.
