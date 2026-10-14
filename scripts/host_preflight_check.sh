#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Host hardware/reachability checks. Prefer the driver:
#   ./scripts/labctl.sh preflight --dpu-ip … --host-ip …
#
# `labctl.sh preflight` passes --skip-opi so this can run *before*
# CloudInfra is up. After `labctl.sh setup-dpu`, omit --skip-opi to
# also confirm CloudInfraService on :50051.
#
set -u

DPU_IP="${DPU_IP:-192.168.100.1}"
HOST_EXPECT_IP="${HOST_MGMT_IP:-192.168.100.2}"
GRPC_PORT="${OPI_GRPC_PORT:-50051}"
GRPC_CLI_IMAGE="${GRPC_CLI_IMAGE:-namely/grpc-cli}"
PING_COUNT="${PING_COUNT:-3}"
PASS=0
FAIL=0
WARN=0
ROWS=()

usage() {
  cat <<'EOF'
Usage: host_preflight_check.sh [options]

  --dpu-ip ADDR          DPU ARM OOB / management IP (default: 192.168.100.1)
  --host-ip ADDR         Host address toward DPU_IP on this machine (OOB/LAN;
                         not tmfifo_net0). Optional sanity check.
  --port N               OPI gRPC port (default: 50051)
  --skip-opi             Skip TCP :50051 and gRPC (use before setup-dpu)
  --skip-grpc            Skip gRPC service list (TCP check still runs)
  --skip-tools           Skip cmake/g++/docker/libsai (those belong to setup-host)
  --skip-lspci           Skip PCIe scan (VMs / no lspci)

Run on the x86/ARM *host*. For the full lab flow use scripts/labctl.sh.
EOF
  exit 1
}

SKIP_GRPC=0
SKIP_OPI=0
SKIP_TOOLS=0
SKIP_LSPCI=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dpu-ip) DPU_IP="$2"; shift 2 ;;
    --host-ip) HOST_EXPECT_IP="$2"; shift 2 ;;
    --port) GRPC_PORT="$2"; shift 2 ;;
    --skip-opi) SKIP_OPI=1; SKIP_GRPC=1; shift ;;
    --skip-grpc) SKIP_GRPC=1; shift ;;
    --skip-tools) SKIP_TOOLS=1; shift ;;
    --skip-lspci) SKIP_LSPCI=1; shift ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
pass() { log "PASS: $*"; PASS=$((PASS + 1)); ROWS+=("PASS|$1|$2"); }
fail() { log "FAIL: $*"; FAIL=$((FAIL + 1)); ROWS+=("FAIL|$1|$2"); }
warn() { log "WARN: $*"; WARN=$((WARN + 1)); ROWS+=("WARN|$1|$2"); }
have_cmd() { command -v "$1" >/dev/null 2>&1; }

# --- PCIe --------------------------------------------------------------------
if [[ "${SKIP_LSPCI}" -eq 1 ]]; then
  warn "PCIe scan" "skipped (--skip-lspci)"
elif ! have_cmd lspci; then
  fail "PCIe scan" "install pciutils (apt-get install pciutils)"
else
  pci="$(lspci -nn 2>/dev/null || lspci 2>/dev/null || true)"
  hit="$(printf '%s\n' "${pci}" | grep -Ei \
    'mellanox|nvidia|bluefield|15b3:|intel.*(ipu|e2000)|ipdk|8086:.*processing|pensando|1dd8:|ionic|smartnic|dpu' \
    || true)"
  if [[ -n "${hit}" ]]; then
    n="$(printf '%s\n' "${hit}" | wc -l | tr -d ' ')"
    pass "PCIe DPU/SmartNIC" "${n} device(s): $(printf '%s' "${hit}" | head -n1)"
  else
    fail "PCIe DPU/SmartNIC" "lspci saw no BlueField/IPU/Pensando; reseat card, update firmware, check dmesg"
  fi
fi

# --- RShim sideband (optional when DPU is on OOB / BMC) ----------------------
# tmfifo_net0 is USB/RShim, NOT the management path to DPU_IP when the card is
# reached over OOB (e.g. 172.22.3.2). Do not require HOST_IP on tmfifo.
rshim_if=""
for cand in tmfifo_net0 rshim0; do
  if have_cmd ip && ip link show "${cand}" >/dev/null 2>&1; then
    rshim_if="${cand}"
    break
  fi
done
if [[ -z "${rshim_if}" ]] && have_cmd ip; then
  rshim_if="$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' \
    | grep -Ei '^(tmfifo|rshim)' | head -n1 || true)"
fi

if [[ -z "${rshim_if}" ]]; then
  warn "RShim interface" "no tmfifo_net0 (OK if DPU ${DPU_IP} is on OOB/BMC)"
else
  state="$(ip -o link show "${rshim_if}" 2>/dev/null | grep -oE 'state [A-Z]+' | awk '{print $2}')"
  rshim_ip="$(ip -4 -o addr show "${rshim_if}" 2>/dev/null | awk '{print $4}' | head -n1)"
  if [[ "${state}" != "UP" && "${state}" != "UNKNOWN" ]]; then
    warn "RShim interface" "${rshim_if} state=${state:-missing} (OK if using OOB for ${DPU_IP})"
  elif [[ -z "${rshim_ip}" ]]; then
    warn "RShim interface" "${rshim_if} UP but no IPv4 (OK if DPU is reached via OOB ${DPU_IP}, not RShim /30)"
  else
    pass "RShim interface" "${rshim_if} ${state} ${rshim_ip}"
  fi
fi

if have_cmd systemctl; then
  if systemctl is-active --quiet rshim 2>/dev/null; then
    pass "rshim service" "active"
  elif [[ -n "${rshim_if}" ]]; then
    warn "rshim service" "interface exists but rshim unit is not active; systemctl enable --now rshim"
  fi
fi

# --- host IP toward DPU (any iface; not tmfifo-specific) --------------------
# HOST_IP / --host-ip is the address on this host used toward DPU_IP (OOB/LAN).
if [[ -n "${HOST_EXPECT_IP}" ]] && have_cmd ip; then
  host_if="$(ip -4 -o addr show 2>/dev/null \
    | awk -v want="${HOST_EXPECT_IP}" '$4 ~ "^"want"/" {print $2; exit}')"
  if [[ -n "${host_if}" ]]; then
    pass "host IP" "${HOST_EXPECT_IP} on ${host_if} (path to DPU ${DPU_IP})"
  else
    warn "host IP" "${HOST_EXPECT_IP} not assigned on this host; set HOST_IP in config/lab.env to a local address toward ${DPU_IP} (not tmfifo)"
  fi
fi

# --- reachability ------------------------------------------------------------
if have_cmd ping; then
  if ping -c "${PING_COUNT}" -W 2 "${DPU_IP}" >/dev/null 2>&1; then
    pass "ping DPU" "${DPU_IP} reachable"
  else
    fail "ping DPU" "${DPU_IP} no reply; check OOB/BMC route, DPU OS, or RShim /30 if that is your only path"
  fi
else
  fail "ping DPU" "ping not in PATH"
fi

tcp_ok=0
if [[ "${SKIP_OPI}" -eq 1 ]]; then
  warn "TCP ${DPU_IP}:${GRPC_PORT}" "skipped (--skip-opi); start CloudInfra with ./scripts/labctl.sh setup-dpu"
else
  if have_cmd nc; then
    if nc -z -w 3 "${DPU_IP}" "${GRPC_PORT}" >/dev/null 2>&1; then
      tcp_ok=1
    fi
  elif have_cmd timeout; then
    if timeout 3 bash -c "echo >/dev/tcp/${DPU_IP}/${GRPC_PORT}" 2>/dev/null; then
      tcp_ok=1
    fi
  elif have_cmd python3; then
    if python3 - "${DPU_IP}" "${GRPC_PORT}" <<'PY' 2>/dev/null
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), 3)
s.close()
PY
    then
      tcp_ok=1
    fi
  fi
  if [[ "${tcp_ok}" -eq 1 ]]; then
    pass "TCP ${DPU_IP}:${GRPC_PORT}" "OPI port open"
  else
    fail "TCP ${DPU_IP}:${GRPC_PORT}" "connection refused; run ./scripts/labctl.sh setup-dpu"
  fi
fi

# --- gRPC --------------------------------------------------------------------
if [[ "${SKIP_GRPC}" -eq 1 ]]; then
  warn "gRPC CloudInfraService" "skipped (--skip-opi/--skip-grpc)"
elif [[ "${tcp_ok}" -ne 1 ]]; then
  fail "gRPC CloudInfraService" "skipped because TCP :${GRPC_PORT} failed"
else
  grpc_out=""
  if have_cmd grpc_cli; then
    grpc_out="$(grpc_cli ls --json_input --json_output "${DPU_IP}:${GRPC_PORT}" -l 2>&1 || true)"
  elif have_cmd grpcurl; then
    grpc_out="$(grpcurl -plaintext "${DPU_IP}:${GRPC_PORT}" list 2>&1 || true)"
  elif have_cmd docker; then
    grpc_out="$(docker run --rm --network=host "${GRPC_CLI_IMAGE}" \
      ls --json_input --json_output "${DPU_IP}:${GRPC_PORT}" -l 2>&1 || true)"
  else
    fail "gRPC CloudInfraService" "need grpc_cli, grpcurl, or docker (${GRPC_CLI_IMAGE})"
    grpc_out=""
  fi
  if printf '%s' "${grpc_out}" | grep -q 'opi_api.network.cloud.v1alpha1.CloudInfraService\|CloudInfraService'; then
    pass "gRPC CloudInfraService" "opi_api.network.cloud.v1alpha1.CloudInfraService at ${DPU_IP}:${GRPC_PORT}"
  elif [[ -n "${grpc_out}" ]]; then
    fail "gRPC CloudInfraService" "listener up but CloudInfraService missing; SNAP on :50051 is storage-only — run ./scripts/labctl.sh setup-dpu"
    printf '%s\n' "${grpc_out}" | tail -n 20
  fi
fi

# --- host toolchain / libsai -------------------------------------------------
if [[ "${SKIP_TOOLS}" -eq 1 ]]; then
  warn "host toolchain" "skipped (--skip-tools); ./scripts/labctl.sh setup-host checks cmake/g++"
else
  for t in docker cmake g++ pkg-config; do
    if have_cmd "${t}"; then
      ver="$("${t}" --version 2>/dev/null | head -n1 || echo present)"
      pass "PATH ${t}" "${ver}"
    else
      fail "PATH ${t}" "install ${t} (build-essential cmake pkg-config docker.io) before compiling the adapter"
    fi
  done

  sai_paths=(/usr/lib/libsai.so /usr/lib/x86_64-linux-gnu/libsai.so /usr/lib/aarch64-linux-gnu/libsai.so)
  found_sai=""
  for p in "${sai_paths[@]}"; do
    if [[ -e "${p}" ]]; then
      found_sai="${p}"
      break
    fi
  done
  if [[ -z "${found_sai}" ]]; then
    warn "libsai.so" "not installed yet (expected before first SONiC/syncd image); install adapter after this pre-flight"
  elif [[ -L "${found_sai}" ]]; then
    tgt="$(readlink -f "${found_sai}" 2>/dev/null || readlink "${found_sai}")"
    if printf '%s' "${tgt}" | grep -q 'opi_adapter'; then
      warn "libsai.so" "${found_sai} already -> ${tgt}; pre-flight is meant to run *before* adapter install"
    else
      pass "libsai.so" "${found_sai} -> ${tgt} (vendor; install_libsai_opi_adapter.sh will replace)"
    fi
  else
    pass "libsai.so" "${found_sai} regular file (vendor copy; installer will back it up)"
  fi
fi

# --- summary -----------------------------------------------------------------
printf '\n'
printf '%-6s %-28s %s\n' "STATUS" "CHECK" "DETAIL"
printf '%-6s %-28s %s\n' "------" "-----" "------"
for row in "${ROWS[@]}"; do
  IFS='|' read -r st name detail <<<"${row}"
  printf '%-6s %-28s %s\n' "${st}" "${name}" "${detail}"
done
printf '\nHost pre-flight: %s passed, %s failed, %s warnings (DPU %s:%s)\n' \
  "${PASS}" "${FAIL}" "${WARN}" "${DPU_IP}" "${GRPC_PORT}"

if [[ "${FAIL}" -ne 0 ]]; then
  cat <<EOF

Remediation:
  PCIe     lspci -nn | grep -iE 'mellanox|bluefield|ipu|pensando'; dmesg | tail
  RShim    optional sideband: apt-get install rshim; systemctl enable --now rshim
           (only if you use the 192.168.100.0/30 tmfifo path — not required for OOB)
  Host IP  HOST_IP in config/lab.env should be a local address toward DPU_IP
           (ip -4 addr), not an address forced onto tmfifo_net0
  Ping     ping ${DPU_IP} — DPU must boot ARM OS; check OOB/BMC (or RShim if that is your path)
  :${GRPC_PORT}  ./scripts/labctl.sh setup-dpu
  gRPC     SNAP storage is not CloudInfra; DASH uses dpu/nvidia or dpu/intel
  Tools    apt-get install build-essential cmake pkg-config docker.io
  Next     ./scripts/labctl.sh setup-host && ./scripts/labctl.sh run
EOF
  exit 1
fi

if [[ "${SKIP_OPI}" -eq 1 ]]; then
  log "host hardware OK; next: ./scripts/labctl.sh setup-dpu --dpu-ip ${DPU_IP}"
else
  log "host is ready; next: ./scripts/labctl.sh setup-host && ./scripts/labctl.sh run --dpu-ip ${DPU_IP}"
fi
exit 0
