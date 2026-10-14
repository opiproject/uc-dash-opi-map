#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Cross-compile linux/arm64 NVIDIA dpu_cloudinfra_bridge on an x86 host.
# Prefer: ./scripts/labctl.sh setup-dpu
# Writes dpu/nvidia/dpu_cloudinfra_bridge for copy onto the DPU.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
OUT="${ROOT}/dpu/nvidia/dpu_cloudinfra_bridge"
IMAGE="${CLOUDINFRA_IMAGE:-dash-opi-cloudinfra:dpu}"

command -v docker >/dev/null 2>&1 || {
  echo "docker is required on the host" >&2
  exit 1
}

echo "building ${IMAGE} (host Go image, GOARCH=arm64 — do not use --platform linux/arm64)"
docker build -f Dockerfile.dpu --build-arg TARGETARCH=arm64 -t "${IMAGE}" .

cid="$(docker create "${IMAGE}")"
docker cp "${cid}:/dpu_cloudinfra_bridge" "${OUT}"
docker rm "${cid}" >/dev/null
chmod +x "${OUT}"

echo "wrote ${OUT}"
if command -v file >/dev/null 2>&1; then
  file "${OUT}"
fi
echo "next: ./scripts/labctl.sh setup-dpu   (copies this binary and starts it)"
