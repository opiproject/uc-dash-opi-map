<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
-->

# Intel IPU CloudInfra daemon (Phase A)

Same `CloudInfraService` + `TelemetryService` RPCs as NVIDIA. Phase A reuses
the NVIDIA OVS/`tc-flower` datapath with Intel ACC netdev names (`enp`, `ens`,
`apf`, `repr`). No IPDK P4 / `infrap4d` yet.

```bash
./scripts/labctl.sh setup-dpu --vendor intel
```

Does **not** speak `BridgePortService` (`intel/ipu-opi-plugins`) or SNAP
`opi-intel-bridge`.
