#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Cross-compile linux/arm64 dpu_intel_bridge on an x86 host.
# Prefer: ./scripts/labctl.sh setup-dpu --vendor intel
# Writes dpu/intel/dpu_intel_bridge for copy onto the ACC.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
OUT="${ROOT}/dpu/intel/dpu_intel_bridge"
IMAGE="${INTEL_IMAGE:-dash-opi-intel:dpu}"

command -v docker >/dev/null 2>&1 || {
  echo "docker is required on the host" >&2
  exit 1
}

echo "building ${IMAGE} (host Go image, GOARCH=arm64 — do not use --platform linux/arm64)"
docker build -f Dockerfile.intel.dpu --build-arg TARGETARCH=arm64 -t "${IMAGE}" .

cid="$(docker create "${IMAGE}")"
docker cp "${cid}:/dpu_intel_bridge" "${OUT}"
docker rm "${cid}" >/dev/null
chmod +x "${OUT}"

echo "wrote ${OUT}"
if command -v file >/dev/null 2>&1; then
  file "${OUT}"
fi
echo "next: ./scripts/labctl.sh setup-dpu --vendor intel"
