#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Run dpu_intel_bridge on the Intel IPU ACC (Phase A OVS CloudInfra).
# Prefer from the host: ./scripts/labctl.sh setup-dpu --vendor intel
# SNAP (opi-intel-bridge) is not started here — DASH uses :50051 only.
#
#   sudo ./scripts/start_intel_cloudinfra_dpu.sh
#
set -euo pipefail

LISTEN="${OPI_GRPC_LISTEN:-:50051}"
VTEP_REMOTE="${OPI_VTEP_REMOTE:-}"
UPLINK="${OPI_UPLINK:-}"
BRIDGE="${OPI_OVS_BRIDGE:-br-opi}"
DRY=0

usage() {
  cat <<'EOF'
Usage: start_intel_cloudinfra_dpu.sh [options]

  --vtep-remote IP   VXLAN tun_dst
  --uplink DEV       uplink netdev (default: auto)
  --listen ADDR      default :50051
  --dry-run          log ip/ovs commands only

Must run on the Intel ACC. Phase A is software OVS, not IPDK P4.
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vtep-remote) VTEP_REMOTE="$2"; shift 2 ;;
    --uplink) UPLINK="$2"; shift 2 ;;
    --listen) LISTEN="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --with-snap) shift ;;
    --no-snap) shift ;;
    --snap-port) shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

native="${ROOT}/dpu/intel/dpu_intel_bridge"
extra=()
[[ -n "${UPLINK}" ]] && extra+=(-uplink "${UPLINK}")
[[ -n "${VTEP_REMOTE}" ]] && extra+=(-vtep-remote "${VTEP_REMOTE}")
[[ "${DRY}" -eq 1 ]] && extra+=(-dry-run)

stop_native() {
  local pidf="/var/run/dpu-intel.pid"
  if [[ -f "${pidf}" ]]; then
    kill "$(cat "${pidf}")" >/dev/null 2>&1 || true
  fi
  pkill -f '/dpu_intel_bridge( |$)' >/dev/null 2>&1 || true
  pkill -x dpu_intel_bridge >/dev/null 2>&1 || true
  if command -v fuser >/dev/null 2>&1; then
    fuser -k 50051/tcp >/dev/null 2>&1 || true
  fi
  sleep 0.5
}

[[ -x "${native}" ]] || die "missing ${native}; run ./scripts/build_intel_arm64.sh on the host"

prep="${ROOT}/scripts/prepare_dpu_datapath.sh"
if [[ -f "${prep}" ]]; then
  chmod +x "${prep}"
  log "preparing OVS before Intel CloudInfra"
  prep_args=(--datapath ovs)
  [[ "${DRY}" -eq 1 ]] && prep_args+=(--dry-run)
  bash "${prep}" "${prep_args[@]}" || die "OVS is not ready on the ACC"
else
  log "WARN: missing ${prep}; CloudInfra will try ovs-vsctl itself"
fi

logf="/var/log/dpu-intel.log"
pidf="/var/run/dpu-intel.pid"
log "running native ${native} in background (log ${logf})"
stop_native
nohup "${native}" -listen "${LISTEN}" -ovs-bridge "${BRIDGE}" "${extra[@]}" \
  >>"${logf}" 2>&1 &
newpid=$!
echo "${newpid}" >"${pidf}"
for i in $(seq 1 20); do
  if ! kill -0 "${newpid}" 2>/dev/null; then
    log "daemon pid=${newpid} exited; ${logf}:"
    tail -n 40 "${logf}" || true
    die "Intel CloudInfra pid ${newpid} died (is :50051 still held?)"
  fi
  if ss -lntp 2>/dev/null | grep -q "pid=${newpid}"; then
    log "Intel CloudInfra on 0.0.0.0:50051 pid=${newpid}  (tail -f ${logf})"
    ss -lntp 2>/dev/null | grep 50051 || true
    exit 0
  fi
  sleep 0.25
done
log "daemon pid=${newpid} running but not in ss; ${logf}:"
tail -n 30 "${logf}" || true
die "new Intel CloudInfra pid ${newpid} did not bind :50051"
