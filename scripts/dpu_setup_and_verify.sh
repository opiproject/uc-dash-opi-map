#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# NVIDIA SNAP storage bring-up (opi-nvidia-bridge): inventory / NVMe / IPsec.
# This is NOT DASH CloudInfra. For DASH from the host use:
#   ./scripts/labctl.sh setup-dpu --dpu-ip … --ssh root@…
#
# Run ON the DPU (BlueField OS / IPDK ARM / Pensando DSC):
#   sudo ./scripts/dpu_setup_and_verify.sh
#   sudo ./scripts/dpu_setup_and_verify.sh --vendor nvidia
#
# From the host over RShim / OOB:
#   ./scripts/dpu_setup_and_verify.sh --ssh ubuntu@192.168.100.1
#
set -u

GRPC_PORT="${OPI_GRPC_PORT:-50051}"
CONTAINER_NAME="${OPI_BRIDGE_NAME:-opi-dpu-bridge}"
GRPC_CLI_IMAGE="${GRPC_CLI_IMAGE:-namely/grpc-cli}"
WAIT_SECONDS="${OPI_BRIDGE_WAIT:-45}"
NETWORK="${OPI_DOCKER_NETWORK:-opi-dpu}"
REDIS_NAME="${OPI_REDIS_NAME:-opi-redis}"
REDIS_IMAGE="${OPI_REDIS_IMAGE:-redis:7.2-alpine}"
VENDOR="${OPI_VENDOR:-auto}"
SOCKET_DIR="${OPI_SOCKET_DIR:-}"
IMAGE="${OPI_BRIDGE_IMAGE:-}"
SSH_TARGET=""
STRICT=0
PASS=0
FAIL=0
WARN=0
ROWS=()

usage() {
  cat <<'EOF'
Usage: dpu_setup_and_verify.sh [options]

  SNAP storage (opi-nvidia-bridge), NOT DASH CloudInfra.
  For DASH from the host: ./scripts/labctl.sh setup-dpu

  --vendor nvidia|intel|amd|auto   DPU family (default: auto-detect)
  --socket-dir PATH                Vendor IPC directory to bind-mount
  --image IMAGE                    OPI bridge image (overrides vendor default)
  --container NAME                 Docker name (default: opi-dpu-bridge)
  --ssh USER@HOST                  Run this script on the DPU over SSH
  --strict                         Fail if vendor sockets are missing
  --no-start                       Audit only; do not docker run the bridge
EOF
  exit 1
}

NO_START=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --vendor) VENDOR="$2"; shift 2 ;;
    --socket-dir) SOCKET_DIR="$2"; shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --container) CONTAINER_NAME="$2"; shift 2 ;;
    --ssh) SSH_TARGET="$2"; shift 2 ;;
    --strict) STRICT=1; shift ;;
    --no-start) NO_START=1; shift ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }

log "SNAP storage path (not DASH CloudInfra). For DASH: ./scripts/labctl.sh setup-dpu"

if [[ -n "${SSH_TARGET}" ]]; then
  self="$(readlink -f "$0" 2>/dev/null || python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$0")"
  extra=()
  [[ "${STRICT}" -eq 1 ]] && extra+=(--strict)
  [[ "${NO_START}" -eq 1 ]] && extra+=(--no-start)
  log "re-exec on ${SSH_TARGET}"
  exec ssh -o StrictHostKeyChecking=accept-new "${SSH_TARGET}" \
    "sudo bash -s -- --vendor ${VENDOR} ${SOCKET_DIR:+--socket-dir ${SOCKET_DIR}} ${IMAGE:+--image ${IMAGE}} --container ${CONTAINER_NAME} ${extra[*]:-}" \
    < "${self}"
fi

pass() { log "PASS: $*"; PASS=$((PASS + 1)); ROWS+=("PASS|$1|$2"); }
fail() { log "FAIL: $*"; FAIL=$((FAIL + 1)); ROWS+=("FAIL|$1|$2"); }
warn() { log "WARN: $*"; WARN=$((WARN + 1)); ROWS+=("WARN|$1|$2"); }

have_cmd() { command -v "$1" >/dev/null 2>&1; }

unit_active() {
  local u="$1"
  have_cmd systemctl || return 1
  systemctl is-active --quiet "$u" 2>/dev/null
}

proc_grep() { pgrep -af "$1" >/dev/null 2>&1; }

detect_vendor() {
  local blob=""
  blob+="$(uname -a 2>/dev/null) "
  blob+="$(cat /etc/os-release 2>/dev/null) "
  [[ -r /sys/class/infiniband ]] && blob+=" mlx "
  have_cmd lspci && blob+="$(lspci 2>/dev/null) "
  blob="$(printf '%s' "${blob}" | tr '[:upper:]' '[:lower:]')"
  if printf '%s' "${blob}" | grep -Eq 'bluefield|mellanox|nvidia|doca|mlx5'; then
    echo nvidia
    return
  fi
  if printf '%s' "${blob}" | grep -Eq 'ipdk|infrap4|intel.*ipu|e2000|mount bryce'; then
    echo intel
    return
  fi
  if printf '%s' "${blob}" | grep -Eq 'pensando|ionic|amd.*dsc|elba'; then
    echo amd
    return
  fi
  echo unknown
}

vendor_defaults() {
  case "$1" in
    nvidia)
      SOCKET_DIR="${SOCKET_DIR:-/var/tmp}"
      IMAGE="${IMAGE:-ghcr.io/opiproject/opi-nvidia-bridge:main}"
      ;;
    intel)
      SOCKET_DIR="${SOCKET_DIR:-/var/run/ipdk}"
      IMAGE="${IMAGE:-ghcr.io/opiproject/opi-intel-bridge:main}"
      ;;
    amd)
      SOCKET_DIR="${SOCKET_DIR:-/var/tmp}"
      IMAGE="${IMAGE:-${OPI_BRIDGE_IMAGE:-}}"
      ;;
    *)
      SOCKET_DIR="${SOCKET_DIR:-/var/tmp}"
      IMAGE="${IMAGE:-ghcr.io/opiproject/opi-nvidia-bridge:main}"
      ;;
  esac
}

# --- OS & driver audit -------------------------------------------------------
kernel="$(uname -s) $(uname -r) $(uname -m)"
if [[ "$(uname -s)" == "Linux" ]] && [[ -r /proc/version ]]; then
  pass "DPU Linux kernel" "${kernel}"
else
  fail "DPU Linux kernel" "not Linux or /proc missing (${kernel})"
fi

if [[ "${VENDOR}" == "auto" ]]; then
  VENDOR="$(detect_vendor)"
  log "detected vendor=${VENDOR}"
fi
case "${VENDOR}" in
  nvidia|intel|amd|unknown|auto) ;;
  *) fail "vendor" "unsupported --vendor ${VENDOR}"; VENDOR=unknown ;;
esac
vendor_defaults "${VENDOR}"
if [[ -z "${IMAGE}" ]]; then
  fail "OPI bridge image" "set --image or OPI_BRIDGE_IMAGE (required for --vendor amd)"
fi

driver_ok=0
case "${VENDOR}" in
  nvidia)
    if lsmod 2>/dev/null | grep -Eq '^mlx5|^ib_core|^nvme'; then driver_ok=1; fi
    for u in doca-telemetry.service doca-telemetry nvidia-peermem.service openvswitch.service; do
      unit_active "$u" && driver_ok=1
    done
    proc_grep 'spdk|doca|mlx5' && driver_ok=1
    [[ -d /sys/class/infiniband ]] && driver_ok=1
    if [[ "${driver_ok}" -eq 1 ]]; then
      pass "NVIDIA DOCA / mlx5 runtime" "modules or DOCA/SPDK units present"
    else
      fail "NVIDIA DOCA / mlx5 runtime" "start DOCA services (systemctl start doca-*) and load mlx5"
    fi
    ;;
  intel)
    if proc_grep 'infrap4d|ipdk|ovs-p4rt|p4rt' || unit_active infrap4d.service; then
      driver_ok=1
      pass "Intel IPDK runtime" "infrap4d / IPDK process active"
    else
      fail "Intel IPDK runtime" "start infrap4d / IPDK before the OPI bridge"
    fi
    ;;
  amd)
    if proc_grep 'ionic|pen_|pensando' || lsmod 2>/dev/null | grep -Eq 'ionic|pen'; then
      driver_ok=1
      pass "AMD Pensando runtime" "ionic / Pensando process or module present"
    else
      fail "AMD Pensando runtime" "start Pensando DSC agents before the OPI bridge"
    fi
    ;;
  *)
    warn "vendor runtime" "could not classify DPU; set --vendor nvidia|intel|amd"
    ;;
esac

# --- sockets -----------------------------------------------------------------
mkdir -p "${SOCKET_DIR}" 2>/dev/null || true
if [[ -d "${SOCKET_DIR}" && -r "${SOCKET_DIR}" && -x "${SOCKET_DIR}" ]]; then
  pass "socket directory" "${SOCKET_DIR} is traversable"
else
  fail "socket directory" "cannot access ${SOCKET_DIR}; mkdir/chmod 0755"
fi

mapfile -t socks < <(find /var/tmp /var/run/doca /var/run/ipdk /var/run/spdk "${SOCKET_DIR}" \
  -name '*.sock' 2>/dev/null | sort -u)
if [[ "${#socks[@]}" -gt 0 ]]; then
  sock_report="$(printf '%s ' "${socks[@]}")"
  bad_mode=0
  for s in "${socks[@]}"; do
    if [[ ! -r "${s}" ]]; then
      bad_mode=1
      warn "socket unreadable" "${s}"
    fi
  done
  if [[ "${bad_mode}" -eq 0 ]]; then
    pass "vendor IPC sockets" "${sock_report}"
  else
    fail "vendor IPC sockets" "chmod 0660 ${SOCKET_DIR}/*.sock so docker (root) can attach"
  fi
else
  msg="no *.sock under ${SOCKET_DIR} /var/tmp /var/run/doca /var/run/ipdk (spdk.sock, DOCA, IPDK)"
  if [[ "${STRICT}" -eq 1 ]]; then
    fail "vendor IPC sockets" "${msg}"
  else
    warn "vendor IPC sockets" "${msg}; bridge may still listen but ASIC programming will fail"
  fi
fi

# --- docker + bridge ---------------------------------------------------------
if ! have_cmd docker; then
  fail "docker" "install Docker on the DPU ARM OS"
else
  pass "docker" "$(docker --version 2>/dev/null | head -n1)"
fi

bridge_running=0
if have_cmd docker; then
  st="$(docker inspect -f '{{.State.Status}} {{.State.Restarting}} {{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null || true)"
  if printf '%s' "${st}" | grep -qi restarting; then
    log "${CONTAINER_NAME} is crash-looping (${st}); will recreate with Redis"
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  elif docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null | grep -qx true; then
    bridge_running=1
    pass "OPI bridge container" "${CONTAINER_NAME} is running"
  fi
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
start_bridge() {
  if [[ "${NO_START}" -eq 1 ]]; then
    warn "OPI bridge start" "skipped (--no-start)"
    return 1
  fi
  if ! have_cmd docker; then
    return 1
  fi
  export OPI_BRIDGE_IMAGE="${IMAGE}"
  export OPI_BRIDGE_NAME="${CONTAINER_NAME}"
  export OPI_SOCKET_DIR="${SOCKET_DIR}"
  export OPI_GRPC_PORT="${GRPC_PORT}"
  if [[ -x "${SCRIPT_DIR}/start_opi_dpu_bridge.sh" ]]; then
    log "launching ${IMAGE} via start_opi_dpu_bridge.sh"
    if "${SCRIPT_DIR}/start_opi_dpu_bridge.sh"; then
      return 0
    fi
    return 1
  fi
  log "pulling ${IMAGE}"
  docker pull "${IMAGE}" || return 1
  docker network inspect "${NETWORK}" >/dev/null 2>&1 || docker network create "${NETWORK}" >/dev/null
  if ! docker inspect -f '{{.State.Running}}' "${REDIS_NAME}" 2>/dev/null | grep -qx true; then
    log "starting ${REDIS_NAME} (${REDIS_IMAGE})"
    docker pull "${REDIS_IMAGE}" || return 1
    docker rm -f "${REDIS_NAME}" >/dev/null 2>&1 || true
    docker run -d --name "${REDIS_NAME}" --restart unless-stopped --network "${NETWORK}" "${REDIS_IMAGE}" || return 1
    local i
    for i in $(seq 1 20); do
      docker exec "${REDIS_NAME}" redis-cli ping 2>/dev/null | grep -q PONG && break
      sleep 1
    done
  fi
  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
  docker run -d \
    --name "${CONTAINER_NAME}" \
    --restart unless-stopped \
    --network "${NETWORK}" \
    -v "${SOCKET_DIR}:/var/tmp/" \
    -p "0.0.0.0:${GRPC_PORT}:50051" \
    "${IMAGE}" \
    /opi-nvidia-bridge \
      -grpc_port=50051 \
      -http_port=8082 \
      -spdk_addr /var/tmp/spdk.sock \
      -redis_addr="${REDIS_NAME}:6379" || return 1
  sleep 3
  if ! docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null | grep -qx true; then
    log "bridge not running; $(docker logs "${CONTAINER_NAME}" 2>&1 | tail -n 20)"
    return 1
  fi
  if docker inspect -f '{{.State.Restarting}}' "${CONTAINER_NAME}" 2>/dev/null | grep -qx true; then
    log "bridge crash-looping; $(docker logs "${CONTAINER_NAME}" 2>&1 | tail -n 20)"
    return 1
  fi
}

if [[ "${bridge_running}" -eq 0 ]]; then
  if [[ "${NO_START}" -eq 1 ]]; then
    fail "OPI bridge container" "${CONTAINER_NAME} not running; re-run without --no-start"
  elif start_bridge; then
    pass "OPI bridge launch" "${CONTAINER_NAME} image=${IMAGE} -v ${SOCKET_DIR}:/var/tmp -p ${GRPC_PORT}:50051"
    bridge_running=1
  else
    fail "OPI bridge launch" "docker run ${IMAGE} failed; docker logs ${CONTAINER_NAME}"
  fi
fi

# --- listener :50051 ---------------------------------------------------------
listen_ok=0
if have_cmd ss; then
  if ss -lntp 2>/dev/null | grep -Eq ":${GRPC_PORT}\\b"; then
    listen_ok=1
  fi
elif have_cmd netstat; then
  if netstat -lntp 2>/dev/null | grep -Eq ":${GRPC_PORT}\\b"; then
    listen_ok=1
  fi
fi

# docker-proxy may show 0.0.0.0:50051 even if the app binds 127.0.0.1 inside.
bind_line=""
if have_cmd ss; then
  bind_line="$(ss -lntp 2>/dev/null | grep -E ":${GRPC_PORT}\\b" || true)"
fi
if [[ "${listen_ok}" -eq 1 ]]; then
  if printf '%s' "${bind_line}" | grep -Eq '0\.0\.0\.0:'"${GRPC_PORT}"'|\[::\]:'"${GRPC_PORT}"'|[*]:'"${GRPC_PORT}"; then
    pass "TCP listener :${GRPC_PORT}" "${bind_line:-ss shows :${GRPC_PORT}}"
  else
    warn "TCP listener :${GRPC_PORT}" "listening but not 0.0.0.0 (host cannot reach DPU); publish -p ${GRPC_PORT}:50051 or --network host"
    printf '%s\n' "${bind_line}"
  fi
else
  fail "TCP listener :${GRPC_PORT}" "ss -lntp has no :${GRPC_PORT}; docker logs ${CONTAINER_NAME}"
fi

grpc_ok=0
if [[ "${listen_ok}" -eq 1 ]] && have_cmd docker; then
  if have_cmd grpc_cli; then
    if grpc_cli ls --json_input --json_output "127.0.0.1:${GRPC_PORT}" -l 2>/dev/null | grep -q CloudInfraService; then
      grpc_ok=1
    fi
  else
    if docker run --rm --network=host "${GRPC_CLI_IMAGE}" \
         ls --json_input --json_output "127.0.0.1:${GRPC_PORT}" -l 2>/dev/null \
         | grep -q CloudInfraService; then
      grpc_ok=1
    fi
  fi
fi
if [[ "${grpc_ok}" -eq 1 ]]; then
  pass "CloudInfraService" "opi_api.network.cloud.v1alpha1.CloudInfraService on 127.0.0.1:${GRPC_PORT}"
elif [[ "${listen_ok}" -eq 1 ]]; then
  warn "CloudInfraService" "TCP up but grpc_cli ls did not list CloudInfraService (install grpc_cli or allow ${GRPC_CLI_IMAGE})"
fi

# --- summary -----------------------------------------------------------------
printf '\n'
printf '%-6s %-28s %s\n' "STATUS" "CHECK" "DETAIL"
printf '%-6s %-28s %s\n' "------" "-----" "------"
for row in "${ROWS[@]}"; do
  IFS='|' read -r st name detail <<<"${row}"
  printf '%-6s %-28s %s\n' "${st}" "${name}" "${detail}"
done
printf '\nDPU bring-up: %s passed, %s failed, %s warnings (vendor=%s image=%s)\n' \
  "${PASS}" "${FAIL}" "${WARN}" "${VENDOR}" "${IMAGE}"

if [[ "${FAIL}" -ne 0 ]]; then
  cat <<EOF

Remediation:
  - Kernel/drivers: vendor BFB/DOCA/IPDK image; systemctl status mlx5 / infrap4d
  - Sockets: start SPDK/DOCA/IPDK first; ls -l ${SOCKET_DIR}/*.sock; chmod 0660
  - Bridge: docker logs ${CONTAINER_NAME}; OPI_BRIDGE_IMAGE=... $0
  - :${GRPC_PORT}: docker run -p ${GRPC_PORT}:50051 and iptables -I INPUT -p tcp --dport ${GRPC_PORT} -j ACCEPT
EOF
  exit 1
fi
exit 0
