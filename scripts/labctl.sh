#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# labctl — DASH / OPI lab controller (run on the host).
#
# Site IPs / SSH live in config/lab.env (gitignored). Do not edit this script
# for lab topology — copy config/lab.env.example → config/lab.env once.
#
#   cp config/lab.env.example config/lab.env
#   ./scripts/labctl.sh preflight
#   ./scripts/labctl.sh setup-dpu              # NVIDIA (default)
#   ./scripts/labctl.sh setup-dpu --vendor intel
#   ./scripts/labctl.sh setup-host
#   ./scripts/labctl.sh run              # C ABI smoke
#   ./scripts/labctl.sh setup-sonic      # gnmi + dashorch + syncd → DPU
#   ./scripts/labctl.sh run-sonic        # gNMI / SAI-Challenger
#   ./scripts/labctl.sh run-iperf        # two ENIs same VNET + iperf3
#   ./scripts/labctl.sh cleanup
#
# Operator cookbook: docs/user-guide.md
#
# Precedence: CLI flags > config/lab.env > defaults below.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

# Repo-safe defaults (generic RShim /30). Overridden by config/lab.env, then CLI.
DPU_IP="192.168.100.1"
HOST_IP="192.168.100.2"
GRPC_PORT="50051"
SSH_TARGET=""
REMOTE_ROOT="~/dash-opi-map"
VTEP_REMOTE=""
SNAP_PORT="50052"
WITH_SNAP=1
DATAPATH="ovs"
KEEP_SNAP=0
KEEP_SONIC=0
HOST_ONLY=0
CMD=""
LAB_ENV_LOADED=0
VENDOR="nvidia"

LAB_ENV="${LAB_ENV:-${ROOT}/config/lab.env}"
if [[ -f "${LAB_ENV}" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "${LAB_ENV}"
  set +a
  LAB_ENV_LOADED=1
fi

# Optional aliases from the environment / lab.env
HOST_IP="${HOST_MGMT_IP:-${HOST_IP}}"
GRPC_PORT="${OPI_GRPC_PORT:-${GRPC_PORT}}"
SNAP_PORT="${OPI_SNAP_PORT:-${SNAP_PORT}}"
VTEP_REMOTE="${OPI_VTEP_REMOTE:-${VTEP_REMOTE}}"
DATAPATH="${OPI_DATAPATH:-${DATAPATH}}"

usage() {
  cat <<'EOF'
Usage: labctl.sh <command> [options]

DASH / OPI lab controller. Run on the host (not on the DPU).
Step-by-step: docs/user-guide.md

Site config (preferred):
  cp config/lab.env.example config/lab.env   # once; gitignored
  ./scripts/labctl.sh all-sonic              # no --dpu-ip flags needed

  preflight     Host PCIe/ping, then read-only DPU inspect. Does not require :50051.
  inspect-dpu   Read-only SoC inventory (OVS, DOCA, representors, listeners). No changes.
  setup-dpu     Prepare OVS or DOCA sidecar, then CloudInfra :50051 on the SoC.
  setup-host    cmake-build libsai_opi_adapter.so and test_sai_create_eni.
  setup-sonic   SONiC control plane (gnmi, APPL_DB, dashorch, syncd) → DPU.
  run           C ABI smoke: sai_create_eni against CloudInfra.
  run-sonic     gNMI / SAI-Challenger against the compose stack.
  run-iperf     Two ENIs in Vnet1 + route/ACL; iperf3 if HOST_VF_A/B are set.
  cleanup       Undo labctl actions. If we stole ports from ovsbr1, put them back.
  all           preflight → setup-dpu → setup-host → run
  all-sonic     preflight → setup-dpu → setup-host → setup-sonic → run-sonic

Options (override config/lab.env for one run):
  --dpu-ip ADDR         DPU ARM IP
  --host-ip ADDR        Host address toward DPU_IP (OOB/LAN; not tmfifo)
  --ssh USER@HOST       SSH target (default: root@<dpu-ip>)
  --vendor nvidia|intel SoC daemon (default: nvidia)
  --port N              CloudInfra gRPC port (default: 50051)
  --vtep-remote IP      VXLAN tun_dst for the DPU daemon
  --datapath ovs|doca|auto  SoC packet backend (default ovs; doca = eSwitch sidecar)
  --no-snap             CloudInfra only on :50051
  --with-snap           Also start SNAP on :50052
  --keep-snap           cleanup: leave SNAP + Redis
  --keep-sonic          cleanup: leave the SONiC compose stack
  --host-only           cleanup: compose down only (no SSH to DPU)

Site config is config/lab.env (DPU prereqs, IPs, DATAPATH). setup-dpu copies it
to the DPU and installs/starts Open vSwitch or the DOCA sidecar from those knobs.

Example:

  cp config/lab.env.example config/lab.env
  ./scripts/labctl.sh all-sonic
  ./scripts/labctl.sh cleanup
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    preflight|inspect-dpu|setup-dpu|setup-host|setup-sonic|run|run-sonic|run-iperf|cleanup|all|all-sonic)
      [[ -z "${CMD}" ]] || { echo "specify one command" >&2; usage; }
      CMD="$1"
      shift
      ;;
    --vendor)
      VENDOR="$2"
      shift 2
      ;;
    --dpu-ip) DPU_IP="$2"; shift 2 ;;
    --host-ip) HOST_IP="$2"; shift 2 ;;
    --ssh) SSH_TARGET="$2"; shift 2 ;;
    --port) GRPC_PORT="$2"; shift 2 ;;
    --vtep-remote) VTEP_REMOTE="$2"; shift 2 ;;
    --datapath) DATAPATH="$2"; shift 2 ;;
    --no-snap) WITH_SNAP=0; shift ;;
    --with-snap) WITH_SNAP=1; shift ;;
    --keep-snap) KEEP_SNAP=1; shift ;;
    --keep-sonic) KEEP_SONIC=1; shift ;;
    --host-only) HOST_ONLY=1; shift ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -n "${CMD}" ]] || usage
[[ -n "${SSH_TARGET}" ]] || SSH_TARGET="root@${DPU_IP}"

COMPOSE_FILE="${ROOT}/deploy/docker-compose.sonic-stack.yml"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }
have_cmd() { command -v "$1" >/dev/null 2>&1; }

write_derived_env() {
  mkdir -p "${ROOT}/build"
  cat >"${ROOT}/build/opi.env" <<EOF
# Generated by labctl from config/lab.env — edit lab.env, not this file.
OPI_SERVER_ADDRESS=${DPU_IP}:${GRPC_PORT}
OPI_SNAP_ADDRESS=${DPU_IP}:${SNAP_PORT}
EOF
  log "wrote ${ROOT}/build/opi.env (OPI_SERVER_ADDRESS=${DPU_IP}:${GRPC_PORT})"
}

VENDOR="$(printf '%s' "${VENDOR}" | tr '[:upper:]' '[:lower:]')"
case "${VENDOR}" in
  nvidia|intel) ;;
  *) die "unknown vendor '${VENDOR}' (use nvidia or intel)" ;;
esac
DATAPATH="$(printf '%s' "${DATAPATH}" | tr '[:upper:]' '[:lower:]')"
case "${DATAPATH}" in
  ovs|doca|auto) ;;
  *) die "unknown datapath '${DATAPATH}' (use ovs, doca, or auto)" ;;
esac
if [[ "${VENDOR}" == "intel" && "${DATAPATH}" == "doca" ]]; then
  die "DOCA Flow is NVIDIA-only; Intel Phase A stays on OVS"
fi

if [[ "${LAB_ENV_LOADED}" -eq 1 ]]; then
  log "lab config ${LAB_ENV} (DPU_IP=${DPU_IP} HOST_IP=${HOST_IP} SSH=${SSH_TARGET} VENDOR=${VENDOR})"
elif [[ ! -f "${ROOT}/config/lab.env.example" ]]; then
  :
else
  log "no ${LAB_ENV}; using defaults (cp config/lab.env.example config/lab.env)"
fi

preflight_bin="${ROOT}/scripts/host_preflight_check.sh"
inspect_bin="${ROOT}/scripts/inspect_dpu.sh"
challenger_bin="${ROOT}/scripts/run_sai_challenger.sh"

compose_cmd() {
  if docker compose version >/dev/null 2>&1; then
    docker compose "$@"
  elif have_cmd docker-compose; then
    docker-compose "$@"
  else
    die "install docker compose (plugin or docker-compose)"
  fi
}

ssh_dpu() {
  ssh -o StrictHostKeyChecking=accept-new "${SSH_TARGET}" "$@"
}

cmd_preflight() {
  log "preflight — host hardware and reachability (CloudInfra not required yet)"
  [[ -f "${preflight_bin}" ]] || die "missing ${preflight_bin}"
  bash "${preflight_bin}" \
    --dpu-ip "${DPU_IP}" \
    --host-ip "${HOST_IP}" \
    --port "${GRPC_PORT}" \
    --skip-opi \
    --skip-tools
  cmd_inspect_dpu
}

cmd_inspect_dpu() {
  log "inspect-dpu — read-only SoC inventory on ${SSH_TARGET} (no install/start)"
  have_cmd ssh || die "ssh is required for inspect-dpu"
  [[ -f "${inspect_bin}" ]] || die "missing ${inspect_bin}"
  bash "${inspect_bin}" \
    --ssh "${SSH_TARGET}" \
    --datapath "${DATAPATH}" \
    --uplink "${DPU_UPLINK:-p0}" \
    --representors "${DPU_HOST_REPRESENTORS:-pf0hpf pf0vf0 pf0vf1}" \
    --port "${GRPC_PORT}" \
    --snap-port "${SNAP_PORT}" \
    --socket "${DPU_DOCA_SOCKET:-/var/run/opi-doca.sock}"
}

wait_grpc() {
  local addr="$1" needle="$2" label="$3"
  local i out
  log "waiting for ${label} at ${addr}"
  for i in $(seq 1 60); do
    out=""
    if have_cmd grpc_cli; then
      out="$(grpc_cli ls --json_input --json_output "${addr}" -l 2>&1 || true)"
    elif have_cmd grpcurl; then
      out="$(grpcurl -plaintext "${addr}" list 2>&1 || true)"
    elif have_cmd docker; then
      out="$(docker run --rm --network=host namely/grpc-cli \
        ls --json_input --json_output "${addr}" -l 2>&1 || true)"
    fi
    if printf '%s' "${out}" | grep -Eq "${needle}"; then
      log "${label} is up at ${addr}"
      return 0
    fi
    sleep 1
  done
  printf '%s\n' "${out}" | tail -n 20 || true
  return 1
}

wait_tcp() {
  local ip="$1" port="$2" label="$3"
  local i
  log "waiting for TCP ${ip}:${port} (${label})"
  for i in $(seq 1 60); do
    if have_cmd nc && nc -z -w 2 "${ip}" "${port}" >/dev/null 2>&1; then
      log "${label} listening on ${ip}:${port}"
      return 0
    fi
    if have_cmd python3 && python3 - "${ip}" "${port}" <<'PY' 2>/dev/null
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), 2)
s.close()
PY
    then
      log "${label} listening on ${ip}:${port}"
      return 0
    fi
    sleep 1
  done
  return 1
}

cmd_setup_dpu() {
  log "setup-dpu — vendor=${VENDOR} datapath=${DATAPATH} CloudInfra :${GRPC_PORT} on ${SSH_TARGET} (install prereqs from lab.env)"
  have_cmd docker || die "docker is required on the host to cross-compile CloudInfra"
  have_cmd ssh || die "ssh is required for setup-dpu"
  have_cmd scp || die "scp is required for setup-dpu"

  local build_cmd start_cmd native remote_dir bin_name start_name snap_on
  if [[ "${VENDOR}" == "intel" ]]; then
    build_cmd="${ROOT}/scripts/build_intel_arm64.sh"
    start_cmd="${ROOT}/scripts/start_intel_cloudinfra_dpu.sh"
    native="${ROOT}/dpu/intel/dpu_intel_bridge"
    remote_dir="${REMOTE_ROOT}/dpu/intel"
    bin_name="dpu_intel_bridge"
    start_name="start_intel_cloudinfra_dpu.sh"
    snap_on=0
  else
    build_cmd="${ROOT}/scripts/build_cloudinfra_arm64.sh"
    start_cmd="${ROOT}/scripts/start_cloudinfra_dpu.sh"
    native="${ROOT}/dpu/nvidia/dpu_cloudinfra_bridge"
    remote_dir="${REMOTE_ROOT}/dpu/nvidia"
    bin_name="dpu_cloudinfra_bridge"
    start_name="start_cloudinfra_dpu.sh"
    snap_on="${WITH_SNAP}"
  fi
  [[ -f "${build_cmd}" ]] || die "missing ${build_cmd}"
  [[ -f "${start_cmd}" ]] || die "missing ${start_cmd}"

  bash "${build_cmd}"
  [[ -f "${native}" ]] || die "build did not write ${native}"
  chmod +x "${native}"

  log "stopping CloudInfra on the DPU so the binary can be replaced"
  ssh_dpu \
    "if [ ! -f /var/run/dpu-cloudinfra.macs ]; then \
       for d in pf0hpf pf1hpf enp3s0f0s0 enp3s0f1s0 en3f0pf0sf0 en3f1pf1sf0; do \
         [ -f /sys/class/net/\$d/address ] && echo \"\$d \$(cat /sys/class/net/\$d/address)\"; \
       done > /var/run/dpu-cloudinfra.macs; \
     fi; \
     if [ -f /var/run/dpu-cloudinfra.pid ]; then kill \"\$(cat /var/run/dpu-cloudinfra.pid)\" 2>/dev/null || true; fi; \
     if [ -f /var/run/dpu-intel.pid ]; then kill \"\$(cat /var/run/dpu-intel.pid)\" 2>/dev/null || true; fi; \
     pkill -f '/dpu_cloudinfra_bridge( |\$)' >/dev/null 2>&1 || true; \
     pkill -x dpu_cloudinfra_ >/dev/null 2>&1 || true; \
     pkill -f '/dpu_intel_bridge( |\$)' >/dev/null 2>&1 || true; \
     command -v fuser >/dev/null && fuser -k 50051/tcp >/dev/null 2>&1 || true; \
     rm -f /var/run/dpu-cloudinfra.pid /var/run/dpu-intel.pid; sleep 0.5; \
     mkdir -p ${remote_dir} ${REMOTE_ROOT}/scripts ${REMOTE_ROOT}/config"

  if [[ -f "${LAB_ENV}" ]]; then
    log "copying lab.env to ${SSH_TARGET}:${REMOTE_ROOT}/config/lab.env"
    scp -q "${LAB_ENV}" "${SSH_TARGET}:${REMOTE_ROOT}/config/lab.env"
  fi

  log "copying ${bin_name} to ${SSH_TARGET}:${remote_dir}"
  scp -q "${native}" \
    "${SSH_TARGET}:${remote_dir}/${bin_name}.new"
  ssh_dpu \
    "mv -f ${remote_dir}/${bin_name}.new ${remote_dir}/${bin_name}"
  scp -q "${start_cmd}" \
    "${SSH_TARGET}:${REMOTE_ROOT}/scripts/${start_name}"
  scp -q "${ROOT}/scripts/prepare_dpu_datapath.sh" \
    "${SSH_TARGET}:${REMOTE_ROOT}/scripts/prepare_dpu_datapath.sh"
  if [[ "${VENDOR}" == "nvidia" ]]; then
    log "copying DOCA sidecar sources"
    ssh_dpu "mkdir -p ${REMOTE_ROOT}/dpu/nvidia/doca ${REMOTE_ROOT}/scripts"
    scp -q "${ROOT}/dpu/nvidia/doca/dpu_doca_flowd.c" \
      "${ROOT}/dpu/nvidia/doca/Makefile" \
      "${ROOT}/dpu/nvidia/doca/README.md" \
      "${SSH_TARGET}:${REMOTE_ROOT}/dpu/nvidia/doca/"
    scp -q "${ROOT}/scripts/start_doca_flowd.sh" \
      "${SSH_TARGET}:${REMOTE_ROOT}/scripts/start_doca_flowd.sh"
  fi
  if [[ "${snap_on}" -eq 1 ]]; then
    [[ -f "${ROOT}/scripts/start_opi_dpu_bridge.sh" ]] || \
      die "missing scripts/start_opi_dpu_bridge.sh (needed for SNAP :${SNAP_PORT})"
    scp -q "${ROOT}/scripts/start_opi_dpu_bridge.sh" \
      "${SSH_TARGET}:${REMOTE_ROOT}/scripts/start_opi_dpu_bridge.sh"
  fi

  local extra=()
  [[ -n "${VTEP_REMOTE}" ]] && extra+=(--vtep-remote "${VTEP_REMOTE}")
  if [[ -n "${DPU_UPLINK:-}" ]]; then
    extra+=(--uplink "${DPU_UPLINK}")
  fi
  if [[ "${VENDOR}" == "nvidia" ]]; then
    extra+=(--datapath "${DATAPATH}")
  fi
  if [[ "${snap_on}" -eq 1 ]]; then
    extra+=(--with-snap --snap-port "${SNAP_PORT}")
  else
    extra+=(--no-snap)
  fi
  [[ "${GRPC_PORT}" != "50051" ]] && extra+=(--listen ":${GRPC_PORT}")
  local start_args=""
  [[ ${#extra[@]} -gt 0 ]] && start_args="${extra[*]}"

  log "starting vendor=${VENDOR} CloudInfra :${GRPC_PORT} on the DPU"
  ssh_dpu \
    "chmod +x ${remote_dir}/${bin_name} \
        ${REMOTE_ROOT}/scripts/${start_name} \
        ${REMOTE_ROOT}/scripts/prepare_dpu_datapath.sh \
        ${REMOTE_ROOT}/scripts/start_doca_flowd.sh \
        ${REMOTE_ROOT}/scripts/start_opi_dpu_bridge.sh 2>/dev/null || true; \
     if [ \"\$(id -u)\" -eq 0 ]; then \
       bash ${REMOTE_ROOT}/scripts/${start_name} ${start_args}; \
     else \
       sudo bash ${REMOTE_ROOT}/scripts/${start_name} ${start_args}; \
     fi"

  wait_grpc "${DPU_IP}:${GRPC_PORT}" "CloudInfraService" "DASH CloudInfra" \
    || die "CloudInfraService did not appear on ${DPU_IP}:${GRPC_PORT}; on DPU: tail -f /var/log/dpu-cloudinfra.log /var/log/dpu-intel.log"

  if [[ "${snap_on}" -eq 1 ]]; then
    wait_tcp "${DPU_IP}" "${SNAP_PORT}" "SNAP" \
      || die "SNAP did not listen on ${DPU_IP}:${SNAP_PORT}; on DPU: docker logs opi-dpu-bridge"
    wait_grpc "${DPU_IP}:${SNAP_PORT}" "InventoryService|Nvme|IPsec|Storage|opi_api" "SNAP storage" \
      || log "WARN: SNAP :${SNAP_PORT} is open but grpc-cli did not list a storage service"
  fi
}

cmd_setup_host() {
  log "setup-host — build libsai_opi_adapter.so + test_sai_create_eni"
  have_cmd cmake || die "install cmake (see README §6.2)"
  have_cmd g++ || die "install g++ / build-essential"

  local cmake_args=(-B build -S .)
  if [[ -d "${OPI_API_ROOT:-$HOME/src/opi-api}" ]]; then
    cmake_args+=(-DOPI_API_ROOT="${OPI_API_ROOT:-$HOME/src/opi-api}")
  fi
  if [[ -d "${GOOGLEAPIS_ROOT:-$HOME/src/googleapis}" ]]; then
    cmake_args+=(-DGOOGLEAPIS_ROOT="${GOOGLEAPIS_ROOT:-$HOME/src/googleapis}")
  fi

  cmake "${cmake_args[@]}"
  cmake --build build -j"$(nproc 2>/dev/null || echo 4)" \
    --target sai_opi_adapter test_sai_create_eni
  ls -l build/libsai_opi_adapter.so build/test_sai_create_eni
}

cmd_setup_sonic() {
  log "setup-sonic — SONiC control plane → ${DPU_IP}:${GRPC_PORT}"
  have_cmd docker || die "docker is required for setup-sonic"
  [[ -f "${ROOT}/build/libsai_opi_adapter.so" ]] || \
    die "build/libsai_opi_adapter.so missing; run: $0 setup-host"
  [[ -f "${COMPOSE_FILE}" ]] || die "missing ${COMPOSE_FILE}"
  [[ -f "${ROOT}/deploy/syncd-lab/lab_sai.c" ]] || die "missing deploy/syncd-lab/lab_sai.c"
  write_derived_env

  # Fail fast: same compile the syncd Dockerfile runs (before a long compose build).
  log "compile-check deploy/syncd-lab/lab_sai.c"
  if have_cmd gcc; then
    gcc -shared -fPIC -Wall -Wextra -o /tmp/liblab_sai.check.so \
      "${ROOT}/deploy/syncd-lab/lab_sai.c" -ldl \
      || die "lab_sai.c failed to compile with host gcc"
    rm -f /tmp/liblab_sai.check.so
  else
    docker run --rm \
      -v "${ROOT}/deploy/syncd-lab:/src:ro" -w /src \
      gcc:12-bookworm \
      gcc -shared -fPIC -Wall -Wextra -o /tmp/liblab_sai.so lab_sai.c -ldl \
      || die "lab_sai.c failed to compile (docker gcc:12)"
  fi
  log "lab_sai.c compile-check OK"

  export ADAPTER_SO="${ROOT}/build/libsai_opi_adapter.so"
  export OPI_SERVER_ADDRESS="${DPU_IP}:${GRPC_PORT}"
  export OPI_SNAP_ADDRESS="${DPU_IP}:${SNAP_PORT}"
  log "OPI_SERVER_ADDRESS=${OPI_SERVER_ADDRESS} OPI_SNAP_ADDRESS=${OPI_SNAP_ADDRESS} ADAPTER_SO=${ADAPTER_SO}"
  compose_cmd -f "${COMPOSE_FILE}" up --build -d --force-recreate

  wait_tcp 127.0.0.1 8080 "sonic-gnmi" || die "sonic-gnmi did not listen on :8080"
  wait_tcp 127.0.0.1 6379 "APPL_DB redis" || die "redis-appl-db did not listen on :6379"
  wait_tcp 127.0.0.1 8090 "dash-gui" || die "dash-gui did not listen on :8090"

  # Host-built adapter must resolve inside the jammy-based syncd image.
  if docker exec syncd ldd /usr/lib/libsai_opi_adapter.so 2>/dev/null | grep -q 'not found'; then
    docker exec syncd ldd /usr/lib/libsai_opi_adapter.so | grep 'not found' || true
    die "adapter .so missing libs inside syncd (rebuild image / match jammy libprotobuf23)"
  fi
  log "compose: sonic-gnmi :8080  dash-gui :8090  redis :6379  dashorch  syncd → ${OPI_SERVER_ADDRESS}"
  log "demo GUI http://127.0.0.1:8090  (gNMI Set of DASH payloads; not grpcui)"
}

cmd_run() {
  log "run — sai_create_eni → CloudInfra at ${DPU_IP}:${GRPC_PORT}"
  local bin="${ROOT}/build/test_sai_create_eni"
  [[ -x "${bin}" ]] || die "build/test_sai_create_eni missing; run: $0 setup-host"
  env -u SAI_OPI_EXPECT_MOCK_STATS \
    "${bin}" "${DPU_IP}:${GRPC_PORT}"
}

cmd_run_sonic() {
  log "run-sonic — gNMI 4×4 create/delete → ${DPU_IP}:${GRPC_PORT}"
  [[ -f "${challenger_bin}" ]] || die "missing ${challenger_bin}"
  write_derived_env
  ADAPTER_SO="${ROOT}/build/libsai_opi_adapter.so" \
    DATAPATH="${DATAPATH}" \
    OPI_DATAPATH="${DATAPATH}" \
    bash "${challenger_bin}" \
      --no-compose \
      --opi "${DPU_IP}:${GRPC_PORT}"
}

cmd_run_iperf() {
  log "run-iperf — two ENIs in Vnet1 + iperf3 overlay (DATAPATH=${DATAPATH})"
  write_derived_env
  if [[ "${DATAPATH}" != "ovs" ]]; then
    log "WARN: same-VNI L2 switching is implemented for DATAPATH=ovs; doca may not hairpin two local ENIs"
  fi
  HOST_VF_A="${HOST_VF_A:-}" HOST_VF_B="${HOST_VF_B:-}" \
    TENANT_IP_A="${TENANT_IP_A:-10.0.0.1}" TENANT_IP_B="${TENANT_IP_B:-10.0.0.2}" \
    IPERF_SECONDS="${IPERF_SECONDS:-10}" \
    GNMI_ADDR="${GNMI_ADDR:-127.0.0.1:8080}" \
    bash "${ROOT}/scripts/run_iperf_overlay.sh"
}

cmd_cleanup() {
  log "cleanup — reverse labctl actions (restore ovsbr1 ports if we stole them)"

  if [[ "${HOST_ONLY}" -eq 0 ]]; then
    have_cmd ssh || die "ssh is required for DPU cleanup (or pass --host-only)"
    local snap_cmd="true"
    if [[ "${KEEP_SNAP}" -eq 0 ]]; then
      snap_cmd="docker rm -f opi-dpu-bridge opi-redis >/dev/null 2>&1 || true"
    fi
    ssh_dpu \
      "if [ -f /var/run/dpu-cloudinfra.pid ]; then kill \"\$(cat /var/run/dpu-cloudinfra.pid)\" 2>/dev/null || true; fi; \
       if [ -f /var/run/dpu-intel.pid ]; then kill \"\$(cat /var/run/dpu-intel.pid)\" 2>/dev/null || true; fi; \
       pkill -f '/dpu_cloudinfra_bridge( |\$)' >/dev/null 2>&1 || true; \
       pkill -x dpu_cloudinfra_ >/dev/null 2>&1 || true; \
       pkill -f '/dpu_intel_bridge( |\$)' >/dev/null 2>&1 || true; \
       pkill -f '/dpu_doca_flowd( |\$)' >/dev/null 2>&1 || true; \
       command -v fuser >/dev/null && fuser -k ${GRPC_PORT}/tcp >/dev/null 2>&1 || true; \
       rm -f /var/run/dpu-cloudinfra.pid /var/run/dpu-intel.pid /var/run/dpu-doca-flowd.pid /var/run/opi-doca.sock; \
       if [ -f /var/run/dpu-cloudinfra.macs ]; then \
         while read -r dev mac; do \
           [ -n \"\$dev\" ] && [ -d /sys/class/net/\$dev ] && ip link set dev \$dev address \$mac 2>/dev/null || true; \
         done < /var/run/dpu-cloudinfra.macs; \
       fi; \
       if command -v ovs-vsctl >/dev/null && ovs-vsctl br-exists br-opi 2>/dev/null; then \
         ovs-vsctl del-br br-opi; \
       fi; \
       if [ -x ${REMOTE_ROOT}/scripts/prepare_dpu_datapath.sh ]; then \
         bash ${REMOTE_ROOT}/scripts/prepare_dpu_datapath.sh --restore-ovsbr1 || true; \
       fi; \
       ${snap_cmd}; \
       echo cleaned-dpu"
    log "DPU: CloudInfra stopped, br-opi removed, representor MACs restored if snapshot existed"
    log "DPU: ovsbr1 ports restored if DPU_STEAL_REPS had taken them"
    [[ "${KEEP_SNAP}" -eq 1 ]] && log "DPU: SNAP left running (--keep-snap)"
  fi

  if [[ "${KEEP_SONIC}" -eq 0 ]]; then
    if have_cmd docker && [[ -f "${COMPOSE_FILE}" ]]; then
      compose_cmd -f "${COMPOSE_FILE}" --profile mock-opi down --remove-orphans || true
      log "host: SONiC compose stack down"
    fi
  else
    log "host: SONiC compose left running (--keep-sonic)"
  fi
}

case "${CMD}" in
  preflight) cmd_preflight ;;
  inspect-dpu) cmd_inspect_dpu ;;
  setup-dpu) cmd_setup_dpu ;;
  setup-host) cmd_setup_host ;;
  setup-sonic) cmd_setup_sonic ;;
  run) cmd_run ;;
  run-sonic) cmd_run_sonic ;;
  run-iperf) cmd_run_iperf ;;
  cleanup) cmd_cleanup ;;
  all)
    cmd_preflight
    cmd_setup_dpu
    cmd_setup_host
    cmd_run
    ;;
  all-sonic)
    cmd_preflight
    cmd_setup_dpu
    cmd_setup_host
    cmd_setup_sonic
    cmd_run_sonic
    ;;
esac

log "done: ${CMD}"
