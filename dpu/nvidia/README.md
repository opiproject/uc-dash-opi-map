<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
-->

# NVIDIA BlueField CloudInfra daemon

This tree **is** the NVIDIA SoC implementation of OPI `CloudInfraService`.
The Go module path stays `dash-opi-map/dpu-cloudinfra` so generated proto
`go_package` values do not churn.

- Binary: `dpu_cloudinfra_bridge` (Linux comm truncated to `dpu_cloudinfra_`)
- Library: `pkg/datapath`, `pkg/server` (imported by Intel Phase A)
- Build: `./scripts/build_cloudinfra_arm64.sh`
- Run on DPU: `./scripts/start_cloudinfra_dpu.sh`

OpenFlow 1.3 on `br-opi` is the **default** backend (`-datapath=ovs`):
table 0 ACL, table 1 L3, table 2 overlay.

Optional **DOCA Flow / eSwitch** (`-datapath=doca`) is the same 16 CloudInfra
RPCs, with Interface / Vnic / Route / SecurityRule programmed by
[`doca/dpu_doca_flowd`](doca/README.md) instead of `br-opi`. The host adapter
talks CloudInfra only — it does not link `libdoca`. Set `DATAPATH=doca` in
[`config/lab.env`](../../config/lab.env.example). Do not mix backends.

Intel Phase A lives in [`../intel/`](../intel/) and imports this module
(OVS only).
