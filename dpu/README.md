<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
-->

# DPU SoC daemons

OPI **`CloudInfraService`** is the vendor-neutral wire API (`:50051`). Each
vendor ships its own process that implements those RPCs.

| Tree | Binary | Hardware | Datapath |
| --- | --- | --- | --- |
| [`nvidia/`](nvidia/) | `dpu_cloudinfra_bridge` | BlueField-2 (validated) | OVS/`br-opi` default; optional DOCA Flow eSwitch sidecar |
| [`intel/`](intel/) | `dpu_intel_bridge` | Intel IPU ACC (Phase A) | ACC `enp`/`ens`/APF + same OVS path |

SNAP storage (`opi-nvidia-bridge`, `opi-intel-bridge`) is **not** these
daemons. Do not point `OPI_SERVER_ADDRESS` at SNAP.

```bash
./scripts/labctl.sh setup-dpu                  # NVIDIA (default)
./scripts/labctl.sh setup-dpu --vendor intel   # Intel Phase A
```
