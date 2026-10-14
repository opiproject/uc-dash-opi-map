#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Read-only inventory of the DPU SoC *before* setup-dpu. Does not install,
# start, or stop anything.
#
#   ./scripts/labctl.sh inspect-dpu
#   ./scripts/inspect_dpu.sh --ssh root@172.22.3.2
#   sudo ./scripts/inspect_dpu.sh          # already on the DPU
#
set -u

DATAPATH="${OPI_DATAPATH:-${DATAPATH:-ovs}}"
UPLINK="${DPU_UPLINK:-${OPI_UPLINK:-p0}}"
REPS="${DPU_HOST_REPRESENTORS:-pf0hpf pf0vf0 pf0vf1}"
GRPC_PORT="${OPI_GRPC_PORT:-${GRPC_PORT:-50051}}"
SNAP_PORT="${OPI_SNAP_PORT:-${SNAP_PORT:-50052}}"
SOCK="${OPI_DOCA_SOCKET:-${DPU_DOCA_SOCKET:-/var/run/opi-doca.sock}}"
HUGE_WANT="${DPU_HUGEPAGES_2MB:-1024}"
SSH_TARGET=""
PASS=0
FAIL=0
WARN=0
INFO=0
ROWS=()
GAPS=()

# Optional lab.env defaults (CLI flags below still win).
_src=""
if [[ -n "${BASH_SOURCE[0]:-}" && -f "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." 2>/dev/null && pwd)/config/lab.env" ]]; then
  _src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/config/lab.env"
elif [[ -f "${HOME}/dash-opi-map/config/lab.env" ]]; then
  _src="${HOME}/dash-opi-map/config/lab.env"
fi
if [[ -n "${_src}" ]]; then
  set -a
  # shellcheck disable=SC1090
  source <(sed 's/\r$//' "${_src}")
  set +a
  DATAPATH="${OPI_DATAPATH:-${DATAPATH:-ovs}}"
  UPLINK="${DPU_UPLINK:-${UPLINK}}"
  REPS="${DPU_HOST_REPRESENTORS:-${REPS}}"
  GRPC_PORT="${OPI_GRPC_PORT:-${GRPC_PORT}}"
  SNAP_PORT="${OPI_SNAP_PORT:-${SNAP_PORT}}"
  SOCK="${OPI_DOCA_SOCKET:-${DPU_DOCA_SOCKET:-${SOCK}}}"
  HUGE_WANT="${DPU_HUGEPAGES_2MB:-${HUGE_WANT}}"
fi
unset _src

usage() {
  cat <<'EOF'
Usage: inspect_dpu.sh [options]

  Read-only DPU SoC check (no apt, no start, no kill).

  --ssh USER@HOST       Run this script on the DPU over SSH
  --datapath ovs|doca|auto
  --uplink DEV
  --representors "pf0hpf pf0vf0 …"
  --port N              CloudInfra port (default 50051)
  --snap-port N         SNAP port (default 50052)
  --socket PATH         DOCA sidecar socket

Prefer: ./scripts/labctl.sh inspect-dpu
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ssh) SSH_TARGET="$2"; shift 2 ;;
    --datapath) DATAPATH="$2"; shift 2 ;;
    --uplink) UPLINK="$2"; shift 2 ;;
    --representors) REPS="$2"; shift 2 ;;
    --port) GRPC_PORT="$2"; shift 2 ;;
    --snap-port) SNAP_PORT="$2"; shift 2 ;;
    --socket) SOCK="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

if [[ -n "${SSH_TARGET}" ]]; then
  self="$(readlink -f "$0" 2>/dev/null || python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$0")"
  echo "inspect-dpu: re-exec on ${SSH_TARGET} (read-only)"
  exec ssh -o StrictHostKeyChecking=accept-new "${SSH_TARGET}" \
    "bash -s -- --datapath $(printf '%q' "${DATAPATH}") --uplink $(printf '%q' "${UPLINK}") --representors $(printf '%q' "${REPS}") --port $(printf '%q' "${GRPC_PORT}") --snap-port $(printf '%q' "${SNAP_PORT}") --socket $(printf '%q' "${SOCK}")" \
    < "${self}"
fi

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
pass() { log "PASS: $1 — $2"; PASS=$((PASS + 1)); ROWS+=("PASS|$1|$2"); }
fail() { log "FAIL: $1 — $2"; FAIL=$((FAIL + 1)); ROWS+=("FAIL|$1|$2"); GAPS+=("$1: $2"); }
warn() { log "WARN: $1 — $2"; WARN=$((WARN + 1)); ROWS+=("WARN|$1|$2"); GAPS+=("$1: $2"); }
info() { log "INFO: $1 — $2"; INFO=$((INFO + 1)); ROWS+=("INFO|$1|$2"); }
have() { command -v "$1" >/dev/null 2>&1; }

DATAPATH="$(printf '%s' "${DATAPATH}" | tr '[:upper:]' '[:lower:]')"
ROOT=""
if [[ -n "${BASH_SOURCE[0]:-}" && "${BASH_SOURCE[0]}" != "bash" && "${BASH_SOURCE[0]}" != "-" ]]; then
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." 2>/dev/null && pwd || true)"
fi
[[ -n "${ROOT}" ]] || ROOT="${HOME}/dash-opi-map"

log "inspect-dpu (read-only) datapath=${DATAPATH} uplink=${UPLINK} grpc=:${GRPC_PORT}"

# --- OS ----------------------------------------------------------------------
kernel="$(uname -s 2>/dev/null || echo unknown) $(uname -r 2>/dev/null || true) $(uname -m 2>/dev/null || true)"
if [[ "$(uname -s 2>/dev/null || true)" == "Linux" ]]; then
  pass "DPU Linux" "${kernel}"
else
  fail "DPU Linux" "not Linux (${kernel}); inspect must run on the SoC"
fi
if [[ "$(uname -m 2>/dev/null || true)" == "aarch64" ]]; then
  pass "arch" "aarch64"
else
  warn "arch" "$(uname -m 2>/dev/null || unknown) (BlueField SoC is usually aarch64)"
fi
if [[ -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  info "OS" "${PRETTY_NAME:-${ID:-unknown}}"
fi

# --- packages / tools --------------------------------------------------------
if have ovs-vsctl && have ovs-ofctl; then
  pass "openvswitch tools" "$(ovs-vsctl --version 2>/dev/null | head -n1)"
else
  if [[ "${DATAPATH}" == "doca" ]]; then
    info "openvswitch tools" "not installed (ok for DATAPATH=doca)"
  else
    warn "openvswitch tools" "missing; setup-dpu will apt-get install openvswitch-switch"
  fi
fi
if have gcc; then
  pass "gcc" "$(gcc --version 2>/dev/null | head -n1)"
else
  if [[ "${DATAPATH}" == "doca" ]]; then
    warn "gcc" "missing; setup-dpu needs it to build dpu_doca_flowd"
  else
    info "gcc" "not installed (only required for DATAPATH=doca)"
  fi
fi
if have ip; then
  pass "iproute2" "$(ip -V 2>/dev/null | head -n1)"
else
  warn "iproute2" "ip not in PATH"
fi
if have docker; then
  pass "docker" "$(docker --version 2>/dev/null | head -n1)"
else
  info "docker" "not installed (needed only if WITH_SNAP=1)"
fi

# --- DOCA SDK ----------------------------------------------------------------
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}:/opt/mellanox/doca/lib/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/doca/lib64/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig"
if pkg-config --exists doca-flow 2>/dev/null; then
  pass "DOCA SDK" "pkg-config doca-flow $(pkg-config --modversion doca-flow 2>/dev/null || echo present)"
elif pkg-config --exists libdoca-sdk-flow 2>/dev/null; then
  pass "DOCA SDK" "pkg-config libdoca-sdk-flow $(pkg-config --modversion libdoca-sdk-flow 2>/dev/null || echo present)"
elif [[ -d /opt/mellanox/doca ]]; then
  pcs="$(find /opt/mellanox/doca /opt/mellanox/dpdk -name '*.pc' 2>/dev/null | head -n 12 | tr '\n' ' ' || true)"
  warn "DOCA SDK" "/opt/mellanox/doca present but no doca-flow.pc (pc: ${pcs:-none})"
else
  if [[ "${DATAPATH}" == "doca" ]]; then
    warn "DOCA SDK" "not found; setup-dpu --datapath doca needs it, or set DPU_DOCA_STUB=1"
  else
    info "DOCA SDK" "not installed (ok for DATAPATH=ovs)"
  fi
fi

# --- hugepages ---------------------------------------------------------------
hp="/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
if [[ -r "${hp}" ]]; then
  cur="$(cat "${hp}" 2>/dev/null || echo 0)"
  if [[ "${DATAPATH}" == "doca" && "${cur}" -lt "${HUGE_WANT}" ]]; then
    warn "hugepages" "${cur} x 2MB (want ≥ ${HUGE_WANT} for DOCA); setup-dpu will raise"
  else
    pass "hugepages" "${cur} x 2MB"
  fi
else
  info "hugepages" "sysfs node missing"
fi
if mountpoint -q /mnt/huge 2>/dev/null; then
  info "hugetlbfs" "/mnt/huge mounted"
fi

# --- netdevs -----------------------------------------------------------------
netdev_state() {
  local d="$1"
  if [[ ! -d "/sys/class/net/${d}" ]]; then
    echo "absent"
    return 1
  fi
  local up
  up="$(cat "/sys/class/net/${d}/operstate" 2>/dev/null || echo unknown)"
  echo "${up}"
}

if [[ -n "${UPLINK}" ]]; then
  st="$(netdev_state "${UPLINK}" || true)"
  if [[ -d "/sys/class/net/${UPLINK}" ]]; then
    pass "uplink ${UPLINK}" "operstate=${st}"
  else
    warn "uplink ${UPLINK}" "netdev missing (set DPU_UPLINK in lab.env)"
  fi
fi
# shellcheck disable=SC2086
for r in ${REPS}; do
  [[ -n "${r}" ]] || continue
  if [[ -d "/sys/class/net/${r}" ]]; then
    st="$(netdev_state "${r}" || true)"
    pass "representor ${r}" "operstate=${st}"
  else
    warn "representor ${r}" "netdev missing"
  fi
done

# --- mlx5 / switchdev --------------------------------------------------------
if [[ -d /sys/class/infiniband ]] || lsmod 2>/dev/null | grep -q '^mlx5'; then
  pass "mlx5" "infiniband and/or mlx5 module present"
else
  warn "mlx5" "no mlx5/infiniband sysfs (unexpected on BlueField)"
fi
if have devlink; then
  esw="$(devlink dev eswitch show 2>/dev/null | tr '\n' ' ' || true)"
  if [[ -n "${esw}" ]]; then
    info "eSwitch" "${esw}"
  fi
fi
if have ovs-vsctl && ovs-vsctl br-exists ovsbr1 2>/dev/null; then
  ports="$(ovs-vsctl list-ports ovsbr1 2>/dev/null | tr '\n' ' ' || true)"
  info "vendor ovsbr1" "exists ports=[${ports}] (setup-dpu leaves this unless DPU_STEAL_REPS=1)"
fi

# --- OVS runtime -------------------------------------------------------------
if pidof ovs-vswitchd >/dev/null 2>&1; then
  pass "ovs-vswitchd" "running pid=$(pidof ovs-vswitchd | tr ' ' ,)"
  if have ovs-vsctl && ovs-vsctl show >/dev/null 2>&1; then
    pass "ovsdb" "ovs-vsctl show ok"
  else
    warn "ovsdb" "ovs-vswitchd up but ovs-vsctl show failed"
  fi
  if have ovs-vsctl && ovs-vsctl br-exists br-opi 2>/dev/null; then
    info "br-opi" "already exists (CloudInfra/setup-dpu will reuse or recreate)"
  else
    info "br-opi" "not created yet (CloudInfra creates it)"
  fi
else
  if [[ "${DATAPATH}" == "doca" ]]; then
    info "ovs-vswitchd" "not running (ok for DATAPATH=doca)"
  else
    warn "ovs-vswitchd" "not running; setup-dpu will start openvswitch"
  fi
fi

# --- CloudInfra / DOCA sidecar / SNAP ----------------------------------------
if pgrep -af '/dpu_cloudinfra_bridge( |$)|dpu_cloudinfra_' >/dev/null 2>&1; then
  info "CloudInfra process" "already running (setup-dpu will replace the binary and restart)"
else
  info "CloudInfra process" "not running"
fi
if pgrep -af '/dpu_intel_bridge( |$)' >/dev/null 2>&1; then
  info "Intel CloudInfra" "dpu_intel_bridge running"
fi
if [[ -S "${SOCK}" ]]; then
  pass "DOCA socket" "${SOCK} exists"
else
  if [[ "${DATAPATH}" == "doca" ]]; then
    warn "DOCA socket" "${SOCK} missing; setup-dpu will start dpu_doca_flowd"
  else
    info "DOCA socket" "${SOCK} not present (ok for ovs)"
  fi
fi
if pgrep -af '/dpu_doca_flowd( |$)' >/dev/null 2>&1; then
  info "dpu_doca_flowd" "already running"
fi

listen_port() {
  local p="$1"
  if have ss && ss -lnt 2>/dev/null | grep -qE ":${p}[[:space:]]"; then
    return 0
  fi
  if have netstat && netstat -lnt 2>/dev/null | grep -qE ":${p}[[:space:]]"; then
    return 0
  fi
  return 1
}

if listen_port "${GRPC_PORT}"; then
  info "TCP :${GRPC_PORT}" "something is listening (CloudInfra or leftover SNAP)"
else
  info "TCP :${GRPC_PORT}" "nothing listening (expected before setup-dpu)"
fi
if listen_port "${SNAP_PORT}"; then
  info "TCP :${SNAP_PORT}" "SNAP or other listener up"
else
  info "TCP :${SNAP_PORT}" "nothing listening"
fi
if have docker && docker inspect -f '{{.State.Running}}' opi-dpu-bridge 2>/dev/null | grep -qx true; then
  ports="$(docker port opi-dpu-bridge 2>/dev/null | tr '\n' ' ' || true)"
  info "SNAP container" "opi-dpu-bridge running (${ports})"
fi

# --- repo copy on DPU --------------------------------------------------------
if [[ -x "${ROOT}/dpu/nvidia/dpu_cloudinfra_bridge" ]]; then
  info "nvidia binary" "${ROOT}/dpu/nvidia/dpu_cloudinfra_bridge present"
elif [[ -x "${HOME}/dash-opi-map/dpu/nvidia/dpu_cloudinfra_bridge" ]]; then
  info "nvidia binary" "${HOME}/dash-opi-map/dpu/nvidia/dpu_cloudinfra_bridge present"
else
  info "nvidia binary" "not on this SoC yet (setup-dpu scp's it)"
fi

# --- summary -----------------------------------------------------------------
printf '\n'
printf '%-6s %-28s %s\n' "STATUS" "CHECK" "DETAIL"
printf '%-6s %-28s %s\n' "------" "-----" "------"
for row in "${ROWS[@]}"; do
  IFS='|' read -r st name detail <<<"${row}"
  printf '%-6s %-28s %s\n' "${st}" "${name}" "${detail}"
done
printf '\nDPU inspect: %s passed, %s failed, %s warnings, %s info (datapath=%s)\n' \
  "${PASS}" "${FAIL}" "${WARN}" "${INFO}" "${DATAPATH}"

if ((${#GAPS[@]})); then
  printf '\nsetup-dpu would still need:\n'
  for g in "${GAPS[@]}"; do
    printf '  - %s\n' "${g}"
  done
else
  printf '\nSoC looks ready. Next: ./scripts/labctl.sh setup-dpu\n'
fi

if [[ "${FAIL}" -ne 0 ]]; then
  exit 1
fi
exit 0
