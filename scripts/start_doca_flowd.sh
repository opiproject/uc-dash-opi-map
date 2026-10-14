#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Start dpu_doca_flowd on the BlueField SoC (Unix socket for CloudInfra -datapath=doca).
# Prefer: ./scripts/labctl.sh setup-dpu --datapath doca
#
set -euo pipefail

SOCK="${OPI_DOCA_SOCKET:-${DPU_DOCA_SOCKET:-/var/run/opi-doca.sock}}"
HUGE="${DPU_HUGEPAGES_2MB:-1024}"
PCI="${DOCA_PCI:-}"
EAL="${DOCA_EAL_ARGS:-}"

usage() {
  cat <<'EOF'
Usage: start_doca_flowd.sh [--socket PATH] [--stub]

  --socket PATH   Unix socket (default /var/run/opi-doca.sock)
  --stub          `make` without libdoca (protocol only)
EOF
  exit 1
}

STUB=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --socket) SOCK="$2"; shift 2 ;;
    --stub) STUB=1; shift ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${ROOT}/dpu/nvidia/doca"
[[ -d "${SRC}" ]] || die "missing ${SRC}"

if [[ -d /opt/mellanox/doca ]]; then
  export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}:/opt/mellanox/doca/lib/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/doca/lib64/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig"
fi

DOCA_MAKE=""
if [[ "${STUB}" -eq 0 ]]; then
  DOCA_MAKE="DOCA=1"
fi

log "building dpu_doca_flowd in ${SRC} ${DOCA_MAKE:-stub}"
# shellcheck disable=SC2086
make -C "${SRC}" ${DOCA_MAKE} || die "build failed (install DOCA SDK for DOCA=1, or pass --stub / DPU_DOCA_STUB=1)"

BIN="${SRC}/dpu_doca_flowd"
[[ -x "${BIN}" ]] || die "missing ${BIN}"

pkill -f '/dpu_doca_flowd( |$)' >/dev/null 2>&1 || true
rm -f "${SOCK}"

if [[ "${STUB}" -eq 0 ]]; then
  if [[ -w /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages ]]; then
    cur="$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)"
    if [[ "${cur}" -lt "${HUGE}" ]]; then
      echo "${HUGE}" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages || \
        log "WARN: could not raise hugepages to ${HUGE}"
    fi
  fi
  mkdir -p /mnt/huge
  mountpoint -q /mnt/huge || mount -t hugetlbfs -o pagesize=2M nodev /mnt/huge || true
fi

extra=()
[[ -n "${PCI}" ]] && extra+=(-pci "${PCI}")
[[ -n "${EAL}" ]] && extra+=(-eal "${EAL}")

logf="/var/log/dpu-doca-flowd.log"
log "starting ${BIN} -s ${SOCK} ${extra[*]:-} (log ${logf})"
nohup "${BIN}" -s "${SOCK}" "${extra[@]}" >>"${logf}" 2>&1 &
echo $! >/var/run/dpu-doca-flowd.pid
sleep 0.3
if [[ ! -S "${SOCK}" ]]; then
  tail -n 30 "${logf}" || true
  die "socket ${SOCK} did not appear"
fi
log "DOCA sidecar on ${SOCK}"
