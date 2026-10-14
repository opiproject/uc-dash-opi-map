#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Build and run dpu_cloudinfra_bridge on the NVIDIA BlueField ARM SoC.
# Prefer from the host: ./scripts/labctl.sh setup-dpu
# Order: prepare datapath (OVS or dpu_doca_flowd) → CloudInfra :50051 → SNAP :50052.
# Do not bind both to 50051.
#
#   sudo ./scripts/start_cloudinfra_dpu.sh
#   sudo ./scripts/start_cloudinfra_dpu.sh --no-snap
#   sudo ./scripts/start_cloudinfra_dpu.sh --vtep-remote 10.0.0.2
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
if [[ -f "${ROOT}/config/lab.env" ]]; then
  set -a
  # shellcheck disable=SC1090
  source <(sed 's/\r$//' "${ROOT}/config/lab.env")
  set +a
fi

IMAGE="${CLOUDINFRA_IMAGE:-dash-opi-cloudinfra:dpu}"
NAME="${CLOUDINFRA_NAME:-opi-cloudinfra}"
LISTEN="${OPI_GRPC_LISTEN:-:50051}"
VTEP_REMOTE="${OPI_VTEP_REMOTE:-${VTEP_REMOTE:-}}"
UPLINK="${OPI_UPLINK:-${DPU_UPLINK:-}}"
BRIDGE="${OPI_OVS_BRIDGE:-${DPU_OVS_BRIDGE:-br-opi}}"
DRY=0
WITH_SNAP="${WITH_SNAP:-1}"
SNAP_PORT="${OPI_SNAP_PORT:-${SNAP_PORT:-50052}}"
DATAPATH="${OPI_DATAPATH:-${DATAPATH:-ovs}}"

usage() {
  cat <<'EOF'
Usage: start_cloudinfra_dpu.sh [options]

  --vtep-remote IP   VXLAN tun_dst (else host representor is L2-forwarded to p0)
  --uplink DEV       uplink netdev (default: auto p0)
  --listen ADDR      default :50051
  --with-snap        default: also start opi-nvidia-bridge on :50052
  --no-snap          CloudInfra only (do not start SNAP)
  --snap-port N      SNAP host port (default 50052)
  --dry-run          log ip/ovs commands only
  --datapath ovs|doca|auto   packet backend (default ovs).
                     ovs: start openvswitch first.
                     doca: start dpu_doca_flowd first.

CloudInfra (DASH) stays on :50051. SNAP cannot share that port; use :50052.
Must run on the DPU. Native binary uses the SoC's ip/ovs; Docker SNAP uses
--network from start_opi_dpu_bridge.sh.
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vtep-remote) VTEP_REMOTE="$2"; shift 2 ;;
    --uplink) UPLINK="$2"; shift 2 ;;
    --listen) LISTEN="$2"; shift 2 ;;
    --with-snap) WITH_SNAP=1; shift ;;
    --no-snap) WITH_SNAP=0; shift ;;
    --snap-port) SNAP_PORT="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --datapath) DATAPATH="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }

# SNAP and CloudInfra cannot both bind 50051. If SNAP is on 50051, stop it
# (optionally recreate on SNAP_PORT after CloudInfra is up).
if command -v docker >/dev/null 2>&1 && \
   docker inspect -f '{{.State.Running}}' opi-dpu-bridge 2>/dev/null | grep -qx true; then
  ports="$(docker port opi-dpu-bridge 2>/dev/null || true)"
  if printf '%s\n' "${ports}" | grep -qE -e ':50051[[:space:]]*$'; then
    log "SNAP opi-dpu-bridge is published on host :50051; stopping so CloudInfra can bind it (SNAP will restart on :${SNAP_PORT})"
    docker stop opi-dpu-bridge >/dev/null || true
  else
    log "SNAP opi-dpu-bridge already off host :50051 (${ports:-no published ports})"
  fi
fi

native="${ROOT}/dpu/nvidia/dpu_cloudinfra_bridge"
extra=()
[[ -n "${UPLINK}" ]] && extra+=(-uplink "${UPLINK}")
[[ -n "${VTEP_REMOTE}" ]] && extra+=(-vtep-remote "${VTEP_REMOTE}")
[[ "${DRY}" -eq 1 ]] && extra+=(-dry-run)
extra+=(-datapath "${DATAPATH}")

prepare_datapath() {
  local helper="${ROOT}/scripts/prepare_dpu_datapath.sh"
  [[ -f "${helper}" ]] || die "missing ${helper}"
  chmod +x "${helper}"
  local prep=(--datapath "${DATAPATH}")
  [[ "${DRY}" -eq 1 ]] && prep+=(--dry-run)
  log "preparing SoC datapath (${DATAPATH}) before CloudInfra"
  bash "${helper}" "${prep[@]}" || die "datapath ${DATAPATH} is not ready"
}

stop_native() {
  local pidf="/var/run/dpu-cloudinfra.pid"
  if [[ -f "${pidf}" ]]; then
    kill "$(cat "${pidf}")" >/dev/null 2>&1 || true
  fi
  # Linux comm is 15 chars, so -x dpu_cloudinfra_bridge never matches dpu_cloudinfra_
  pkill -f '/dpu_cloudinfra_bridge( |$)' >/dev/null 2>&1 || true
  pkill -x dpu_cloudinfra_ >/dev/null 2>&1 || true
  if command -v fuser >/dev/null 2>&1; then
    fuser -k 50051/tcp >/dev/null 2>&1 || true
  fi
  sleep 0.5
}

run_native() {
  local logf="/var/log/dpu-cloudinfra.log"
  local pidf="/var/run/dpu-cloudinfra.pid"
  log "running native ${native} in background (log ${logf})"
  stop_native
  nohup "${native}" -listen "${LISTEN}" -ovs-bridge "${BRIDGE}" "${extra[@]}" \
    >>"${logf}" 2>&1 &
  local newpid=$!
  echo "${newpid}" >"${pidf}"
  local i
  for i in $(seq 1 20); do
    if ! kill -0 "${newpid}" 2>/dev/null; then
      log "daemon pid=${newpid} exited; ${logf}:"
      tail -n 40 "${logf}" || true
      if [[ -f /var/log/dpu-doca-flowd.log ]]; then
        log "dpu-doca-flowd.log (last 40):"
        tail -n 40 /var/log/dpu-doca-flowd.log || true
      fi
      die "CloudInfra pid ${newpid} died (is :50051 still held by an old process?)"
    fi
    if ss -lntp 2>/dev/null | grep -q "pid=${newpid}"; then
      log "CloudInfra on 0.0.0.0:50051 pid=${newpid}  (tail -f ${logf})"
      ss -lntp 2>/dev/null | grep 50051 || true
      return 0
    fi
    sleep 0.25
  done
  log "daemon pid=${newpid} running but not in ss; ${logf}:"
  tail -n 30 "${logf}" || true
  ss -lntp 2>/dev/null | grep 50051 || true
  die "new CloudInfra pid ${newpid} did not bind :50051"
}

start_snap_if_requested() {
  [[ "${WITH_SNAP}" -eq 1 ]] || return 0
  command -v docker >/dev/null 2>&1 || \
    die "SNAP on :${SNAP_PORT} needs docker on the DPU (or pass --no-snap)"
  local helper="${ROOT}/scripts/start_opi_dpu_bridge.sh"
  [[ -f "${helper}" ]] || die "missing ${helper} (needed for SNAP :${SNAP_PORT})"
  chmod +x "${helper}"
  log "starting SNAP opi-nvidia-bridge on 0.0.0.0:${SNAP_PORT} (storage/inventory/ipsec)"
  OPI_GRPC_PORT="${SNAP_PORT}" OPI_HTTP_PORT="${OPI_HTTP_PORT:-8082}" \
    bash "${helper}" || die "SNAP failed on :${SNAP_PORT}; CloudInfra is on :50051. docker logs opi-dpu-bridge"
}

if [[ -x "${native}" ]]; then
  prepare_datapath
  run_native
  start_snap_if_requested
  exit 0
fi

command -v docker >/dev/null 2>&1 || die "docker is required (or copy an arm64 dpu_cloudinfra_bridge under dpu/nvidia/)"

log "building ${IMAGE} from Dockerfile.dpu"
docker build -f Dockerfile.dpu -t "${IMAGE}" .

docker rm -f "${NAME}" >/dev/null 2>&1 || true
prepare_datapath

args=(
  -d --name "${NAME}"
  --restart unless-stopped
  --network host
  --privileged
  -v /var/run/openvswitch:/var/run/openvswitch
  -v /sys/class/net:/sys/class/net:ro
  "${IMAGE}"
  -listen "${LISTEN}"
  -ovs-bridge "${BRIDGE}"
)
[[ -n "${UPLINK}" ]] && args+=(-uplink "${UPLINK}")
[[ -n "${VTEP_REMOTE}" ]] && args+=(-vtep-remote "${VTEP_REMOTE}")
[[ "${DRY}" -eq 1 ]] && args+=(-dry-run)
args+=(-datapath "${DATAPATH}")

log "starting ${NAME} listen=${LISTEN} bridge=${BRIDGE}"
docker run "${args[@]}"

sleep 2
if ! docker inspect -f '{{.State.Running}}' "${NAME}" 2>/dev/null | grep -qx true; then
  docker logs "${NAME}" 2>&1 | tail -n 40
  die "container ${NAME} is not running"
fi
log "CloudInfra on 0.0.0.0:50051  (docker logs ${NAME})"
ss -lntp 2>/dev/null | grep 50051 || true
docker logs --tail 20 "${NAME}" || true
start_snap_if_requested
