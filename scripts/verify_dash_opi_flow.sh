#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# End-to-end check: SONiC APPL_DB DASH objects → libsai_opi_adapter gRPC
# translation logs → ENI (Vnic) and VNET (Vpc) present on the DPU.
#
#   sudo ./scripts/verify_dash_opi_flow.sh \
#       --opi-server 192.168.100.1:50051
set -euo pipefail

OPI_SERVER_ADDRESS="${OPI_SERVER_ADDRESS:-192.168.100.1:50051}"
SYNCD_CONTAINER="${SYNCD_CONTAINER:-syncd}"
DATABASE_CONTAINER="${DATABASE_CONTAINER:-database}"
WAIT_SECONDS="${WAIT_SECONDS:-60}"
GRPC_CLI_IMAGE="${GRPC_CLI_IMAGE:-namely/grpc-cli}"

VNET_NAME="${VNET_NAME:-Vnet1}"
VNI="${VNI:-45654}"
QOS_NAME="${QOS_NAME:-qos1}"
CPS="${CPS:-10000}"
ENI_KEY="${ENI_KEY:-F4939FEFC47E}"
ENI_MAC="${ENI_MAC:-F4-93-9F-EF-C4-7E}"
ENI_UUID="${ENI_UUID:-497f23d7-f0ac-4c99-a98f-59b470e8c7bd}"
UNDERLAY_IP="${UNDERLAY_IP:-25.1.1.1}"

SERVICE="opi_api.network.cloud.v1alpha1.CloudInfraService"
WORKDIR=""
PASS=0
FAIL=0

cleanup() {
  if [[ "${FAIL}" -eq 0 && -n "${WORKDIR}" ]]; then
    rm -rf "${WORKDIR}"
  elif [[ -n "${WORKDIR}" ]]; then
    log "keeping ${WORKDIR} for inspection"
  fi
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage: verify_dash_opi_flow.sh [options]

  --opi-server HOST:PORT   DPU CloudInfraService (default: 192.168.100.1:50051)
  --syncd NAME             syncd docker container (default: syncd)
  --wait SECONDS           time to wait for adapter logs (default: 60)
  --vnet NAME              DASH_VNET_TABLE key (default: Vnet1)
  --vni N                  VXLAN VNI (default: 45654)
  --eni-mac MAC            DASH ENI MAC (default: F4-93-9F-EF-C4-7E)
  --cps N                  QoS CPS → VnicSpec.max_cps (default: 10000)
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --opi-server) OPI_SERVER_ADDRESS="$2"; shift 2 ;;
    --syncd) SYNCD_CONTAINER="$2"; shift 2 ;;
    --wait) WAIT_SECONDS="$2"; shift 2 ;;
    --vnet) VNET_NAME="$2"; shift 2 ;;
    --vni) VNI="$2"; shift 2 ;;
    --eni-mac) ENI_MAC="$2"; ENI_KEY="${2//[-:]/}"; ENI_KEY="${ENI_KEY^^}"; shift 2 ;;
    --cps) CPS="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
pass() { log "PASS: $*"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $*"; FAIL=$((FAIL + 1)); }

WORKDIR="$(mktemp -d /tmp/dash-opi-verify.XXXXXX)"
LOG_SNAP="${WORKDIR}/adapter.log"

# --- APPL_DB -----------------------------------------------------------------
appl_db() {
  if command -v sonic-db-cli >/dev/null 2>&1; then
    sonic-db-cli APPL_DB "$@"
  elif command -v docker >/dev/null 2>&1 && docker inspect "${DATABASE_CONTAINER}" >/dev/null 2>&1; then
    docker exec "${DATABASE_CONTAINER}" redis-cli -n 0 "$@"
  elif command -v redis-cli >/dev/null 2>&1; then
    redis-cli -n 0 "$@"
  else
    echo "need sonic-db-cli, redis-cli, or a '${DATABASE_CONTAINER}' container" >&2
    return 1
  fi
}

push_dash_config() {
  log "pushing DASH VNET/QoS/ENI into APPL_DB"
  appl_db HMSET "DASH_VNET_TABLE:${VNET_NAME}" \
    vni "${VNI}" \
    guid "559c6ce8-26ab-4193-b946-ccc6e8f930b2" \
    version "1"

  appl_db HMSET "DASH_QOS_TABLE:${QOS_NAME}" \
    qos_id "${QOS_NAME}" \
    cps "${CPS}" \
    flows "1000" \
    bw "1000000"

  appl_db HMSET "DASH_ENI_TABLE:${ENI_KEY}" \
    eni_id "${ENI_UUID}" \
    mac_address "${ENI_MAC}" \
    qos "${QOS_NAME}" \
    underlay_ip "${UNDERLAY_IP}" \
    admin_state enabled \
    vnet "${VNET_NAME}" \
    mode vm_mode

  log "APPL_DB DASH_VNET_TABLE:${VNET_NAME} vni=${VNI}"
  log "APPL_DB DASH_ENI_TABLE:${ENI_KEY} mac=${ENI_MAC} vnet=${VNET_NAME} qos=${QOS_NAME}"
}

# --- adapter translation logs (host + syncd) ---------------------------------
collect_adapter_logs() {
  : > "${LOG_SNAP}"
  for f in /var/log/sai-opi-adapter.log /tmp/sai-opi-adapter.log; do
    [[ -r "${f}" ]] && cat "${f}" >> "${LOG_SNAP}" || true
  done
  if command -v journalctl >/dev/null 2>&1; then
    journalctl -t sai-opi-adapter --since "5 min ago" --no-pager 2>/dev/null >> "${LOG_SNAP}" || true
  fi
  if command -v docker >/dev/null 2>&1 && docker inspect "${SYNCD_CONTAINER}" >/dev/null 2>&1; then
    docker logs --since 5m "${SYNCD_CONTAINER}" 2>&1 | grep -E 'sai-opi' >> "${LOG_SNAP}" || true
    docker exec "${SYNCD_CONTAINER}" sh -c \
      'cat /var/log/sai-opi-adapter.log /tmp/sai-opi-adapter.log 2>/dev/null' \
      >> "${LOG_SNAP}" || true
  fi
}

wait_for_translation() {
  log "waiting up to ${WAIT_SECONDS}s for sai-opi CreateVnic log"
  local deadline=$((SECONDS + WAIT_SECONDS))
  while (( SECONDS < deadline )); do
    collect_adapter_logs
    if grep -q 'sai-opi: CreateVnic' "${LOG_SNAP}"; then
      return 0
    fi
    sleep 2
  done
  return 1
}

mac_colon="$(echo "${ENI_MAC}" | tr 'A-F' 'a-f' | tr '-' ':')"
mac_compact="$(echo "${ENI_KEY}" | tr 'A-F' 'a-f')"

assert_translation_logs() {
  log "checking DASH → OPI fields in adapter gRPC logs"
  cp "${LOG_SNAP}" "${WORKDIR}/adapter.final.log"
  if grep -Eiq "CreateVnic" "${LOG_SNAP}"; then
    pass "CreateVnic RPC was issued"
  else
    fail "no CreateVnic line in adapter logs"
  fi
  if grep -Eiq "mac=${mac_colon}|mac=.*${mac_compact}" "${LOG_SNAP}"; then
    pass "ENI MAC translated into VnicSpec.mac_address (${mac_colon})"
  else
    fail "adapter log missing mac=${mac_colon}"
  fi
  if grep -Eq "max_cps=${CPS}" "${LOG_SNAP}"; then
    pass "QoS CPS translated into VnicSpec.max_cps=${CPS}"
  else
    fail "adapter log missing max_cps=${CPS}"
  fi
  if grep -Eq "CreateInterface" "${LOG_SNAP}"; then
    pass "CreateInterface RPC was issued (HostIfSpec)"
  else
    fail "no CreateInterface line (expected for VM-mode ENI)"
  fi
  if grep -Eq "subnet_name_ref=" "${LOG_SNAP}"; then
    pass "VNET id mapped to VnicSpec.subnet_name_ref"
  else
    fail "adapter log missing subnet_name_ref (VNET → subnet/Vpc)"
  fi
}

# --- DPU CloudInfraService ---------------------------------------------------
grpc_try() {
  local method="$1"
  local payload="${2:-{}}"
  local out="${WORKDIR}/grpc-${method}.out"
  local fq="${SERVICE}.${method}"

  if command -v opi-cli >/dev/null 2>&1; then
    if opi-cli --addr "${OPI_SERVER_ADDRESS}" "${method}" "${payload}" \
         >"${out}" 2>"${out}.err"; then
      cat "${out}"
      return 0
    fi
  fi

  if command -v grpc_cli >/dev/null 2>&1; then
    if grpc_cli call --json_input --json_output "${OPI_SERVER_ADDRESS}" \
         "${fq}" "${payload}" >"${out}" 2>"${out}.err"; then
      cat "${out}"
      return 0
    fi
  fi

  if command -v grpcurl >/dev/null 2>&1; then
    if grpcurl -plaintext -d "${payload}" "${OPI_SERVER_ADDRESS}" \
         "${SERVICE}/${method}" >"${out}" 2>"${out}.err"; then
      cat "${out}"
      return 0
    fi
  fi

  if command -v docker >/dev/null 2>&1; then
    if docker run --rm --network=host "${GRPC_CLI_IMAGE}" \
         call --json_input --json_output "${OPI_SERVER_ADDRESS}" \
         "${fq}" "${payload}" >"${out}" 2>"${out}.err"; then
      cat "${out}"
      return 0
    fi
  fi

  log "grpc ${method} failed (need opi-cli, grpc_cli, grpcurl, or ${GRPC_CLI_IMAGE})"
  [[ -s "${out}.err" ]] && cat "${out}.err" >&2 || true
  return 1
}

assert_dpu_hardware() {
  log "querying DPU gRPC ${OPI_SERVER_ADDRESS}"

  local vnics vpcs
  vnics="$(grpc_try ListVnics '{"parent":""}' || true)"
  vpcs="$(grpc_try ListVpcs '{"parent":""}' || true)"

  local vnic_name
  vnic_name="$(sed -n 's/.*CreateVnic name=\([^ ]*\).*/\1/p' "${LOG_SNAP}" | tail -n1 || true)"
  if [[ -n "${vnic_name}" ]]; then
    grpc_try GetVnic "{\"name\":\"${vnic_name}\"}" >/dev/null || true
  fi

  local blob
  blob="$(printf '%s\n%s\n' "${vnics}" "${vpcs}")"
  printf '%s\n' "${blob}" > "${WORKDIR}/dpu-query.txt"

  if grep -Eiq "${mac_colon}|${mac_compact}|${ENI_KEY}|${vnic_name:-__none__}" \
       "${WORKDIR}/dpu-query.txt" "${WORKDIR}"/grpc-GetVnic.out 2>/dev/null; then
    pass "DPU has ENI/Vnic matching MAC ${ENI_MAC}"
  else
    fail "DPU ListVnics/GetVnic has no ENI MAC ${ENI_MAC} or ${vnic_name:-vnic name}"
  fi

  if grep -Eq "${VNI}|vnid.:.?${VNI}|fabric_encap" "${WORKDIR}/dpu-query.txt" \
       "${WORKDIR}"/grpc-ListVpcs.out 2>/dev/null; then
    pass "DPU has VNET/Vpc with VNI ${VNI}"
  else
    fail "DPU ListVpcs has no Vpc vnid=${VNI} (VNET ${VNET_NAME})"
  fi
}

# --- run ---------------------------------------------------------------------
log "OPI_SERVER_ADDRESS=${OPI_SERVER_ADDRESS}"
push_dash_config

if wait_for_translation; then
  pass "saw sai-opi translation log after APPL_DB push"
else
  fail "timed out waiting for libsai_opi_adapter CreateVnic log"
  collect_adapter_logs
fi

assert_translation_logs
assert_dpu_hardware

log "results: ${PASS} passed, ${FAIL} failed"
if [[ "${FAIL}" -ne 0 ]]; then
  log "---- adapter log ----"
  cat "${LOG_SNAP}" || true
  exit 1
fi
exit 0
