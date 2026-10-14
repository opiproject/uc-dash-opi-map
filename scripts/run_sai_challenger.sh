#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Drive the containerized SONiC stack with gnmi-cli/gnmic (DASH YANG/JSON)
# and SAI-Challenger (OCP northbound pytest + optional saivs clone).
#
# End-to-end path verified:
#   gNMI Set → APPL_DB → dashorch → ASIC workq → syncd → libsai_opi_adapter.so
#   → OPI gRPC (DPU or --profile mock-opi)
#
#   ./scripts/run_sai_challenger.sh
#   ./scripts/run_sai_challenger.sh --compose-up --profile mock-opi
#   ./scripts/run_sai_challenger.sh --no-compose   # stack already running
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE_FILE="${COMPOSE_FILE:-${ROOT}/deploy/docker-compose.sonic-stack.yml}"
COMPOSE=(docker compose -f "${COMPOSE_FILE}")
GNMI_ADDR="${GNMI_ADDR:-127.0.0.1:8080}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
OPI_SERVER_ADDRESS="${OPI_SERVER_ADDRESS:-127.0.0.1:50051}"
GNMIC_IMAGE="${GNMIC_IMAGE:-ghcr.io/openconfig/gnmic:0.38.2}"
SAI_CHALLENGER_DIR="${SAI_CHALLENGER_DIR:-${ROOT}/build/SAI-Challenger}"
SAI_CHALLENGER_REPO="${SAI_CHALLENGER_REPO:-https://github.com/opencomputeproject/SAI-Challenger.git}"
PROFILE_ARGS=()
COMPOSE_UP=1
WAIT_SECONDS="${WAIT_SECONDS:-90}"
PASS=0
FAIL=0
REPORT_DIR="${REPORT_DIR:-${ROOT}/build/reports}"
REPORT_MD="${REPORT_MD:-${REPORT_DIR}/run-sonic-latest.md}"
REPORT_JSON="${REPORT_JSON:-${REPORT_DIR}/run-sonic-latest.json}"
RPC_LINES=()

usage() {
  cat <<'EOF'
Usage: run_sai_challenger.sh [options]

  --compose-up / --no-compose   Start (default) or skip docker compose
  --profile mock-opi            Also start the in-tree OPI CloudInfra mock
  --gnmi HOST:PORT              sonic-gnmi (default 127.0.0.1:8080)
  --opi HOST:PORT               OPI server (default 127.0.0.1:50051)
  --sai-challenger-dir PATH     Clone/build location for OCP SAI-Challenger
  --report PATH                Markdown report (default build/reports/run-sonic-latest.md)

Scores the 4×4 CloudInfra matrix (VNET / ENI / Route / ACL × Create / Delete).
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --compose-up) COMPOSE_UP=1; shift ;;
    --no-compose) COMPOSE_UP=0; shift ;;
    --profile) PROFILE_ARGS+=(--profile "$2"); shift 2 ;;
    --gnmi) GNMI_ADDR="$2"; shift 2 ;;
    --opi) OPI_SERVER_ADDRESS="$2"; shift 2 ;;
    --sai-challenger-dir) SAI_CHALLENGER_DIR="$2"; shift 2 ;;
    --report) REPORT_MD="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
pass() { log "PASS: $*"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $*"; FAIL=$((FAIL + 1)); }

note_rpc() {
  local rpc="$1" status="$2" detail="${3:-}"
  RPC_LINES+=("${rpc}|${status}|${detail}")
}

syncd_blob() {
  docker logs syncd 2>&1 || true
}

score_rpc() {
  local rpc="$1"
  local blob
  blob="$(syncd_blob)"
  if echo "${blob}" | grep -q "${rpc}"; then
    pass "CloudInfra ${rpc}"
    note_rpc "${rpc}" "PASS" "syncd log"
  else
    fail "CloudInfra ${rpc} missing in syncd logs"
    note_rpc "${rpc}" "FAIL" "not in docker logs syncd"
    if [[ "${rpc}" == DeleteVnic || "${rpc}" == DeleteInterface ]]; then
      echo "${blob}" | grep -E 'sai_remove_eni |CreateVnic |CreateInterface ' | tail -n 8 || true
    fi
  fi
}

write_report() {
  [[ "${REPORT_WRITTEN:-0}" -eq 1 ]] && return 0
  REPORT_WRITTEN=1
  mkdir -p "$(dirname "${REPORT_MD}")"
  local ts line rpc st detail
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  {
    echo "# run-sonic report"
    echo
    echo "- generated: ${ts}"
    echo "- OPI: ${OPI_SERVER_ADDRESS}"
    echo "- datapath: ${DATAPATH:-${OPI_DATAPATH:-ovs}}"
    echo "- results: ${PASS} passed, ${FAIL} failed"
    echo
    echo "## 4×4 CloudInfra RPCs"
    echo
    echo "| Object | Create | Delete |"
    echo "| --- | --- | --- |"
    echo "| VNET | CreateVpc, CreateSubnet | DeleteSubnet, DeleteVpc |"
    echo "| ENI | CreateInterface, CreateVnic | DeleteVnic, DeleteInterface |"
    echo "| Route | CreateRouteTable, CreateRoute | DeleteRoute, DeleteRouteTable |"
    echo "| ACL | CreateSecurityPolicy, CreateSecurityRule | DeleteSecurityRule, DeleteSecurityPolicy |"
    echo
    echo "| RPC | Result | Detail |"
    echo "| --- | --- | --- |"
    if ((${#RPC_LINES[@]})); then
      for line in "${RPC_LINES[@]}"; do
        rpc="${line%%|*}"
        rest="${line#*|}"
        st="${rest%%|*}"
        detail="${rest#*|}"
        echo "| \`${rpc}\` | ${st} | ${detail} |"
      done
    fi
  } >"${REPORT_MD}"

  {
    echo "{"
    echo "  \"generated\": \"${ts}\","
    echo "  \"opi\": \"${OPI_SERVER_ADDRESS}\","
    echo "  \"datapath\": \"${DATAPATH:-${OPI_DATAPATH:-ovs}}\","
    echo "  \"passed\": ${PASS},"
    echo "  \"failed\": ${FAIL},"
    echo "  \"rpcs\": ["
    local first=1
    if ((${#RPC_LINES[@]})); then
      for line in "${RPC_LINES[@]}"; do
        rpc="${line%%|*}"
        rest="${line#*|}"
        st="${rest%%|*}"
        detail="${rest#*|}"
        [[ "${first}" -eq 1 ]] || echo ","
        first=0
        printf '    {"rpc": "%s", "result": "%s", "detail": "%s"}' "${rpc}" "${st}" "${detail}"
      done
    fi
    echo
    echo "  ]"
    echo "}"
  } >"${REPORT_JSON}"
  log "report ${REPORT_MD}"
  log "report ${REPORT_JSON}"
}

wait_tcp() {
  local addr="$1" timeout="${2:-${WAIT_SECONDS}}"
  local host="${addr%:*}" port="${addr##*:}"
  local deadline=$((SECONDS + timeout))
  while (( SECONDS < deadline )); do
    if command -v nc >/dev/null 2>&1 && nc -z "${host}" "${port}" 2>/dev/null; then
      return 0
    fi
    if command -v python3 >/dev/null 2>&1; then
      python3 - "${host}" "${port}" <<'PY' && return 0
import socket, sys
s = socket.socket(); s.settimeout(0.5)
try:
    s.connect((sys.argv[1], int(sys.argv[2]))); sys.exit(0)
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
    fi
    sleep 0.4
  done
  return 1
}

redis_cli() {
  if docker inspect redis-appl-db >/dev/null 2>&1; then
    docker exec redis-appl-db redis-cli "$@"
  else
    command -v redis-cli >/dev/null && redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" "$@"
  fi
}

gnmi_set() {
  local path="$1" file="$2"
  if command -v gnmic >/dev/null 2>&1; then
    gnmic -a "${GNMI_ADDR}" --insecure --encoding json_ietf \
      set --replace-path "${path}" --replace-file "${file}"
    return
  fi
  if command -v gnmi_cli >/dev/null 2>&1; then
    # openconfig/gnmi gnmi_cli (when present)
    gnmi_cli -address "${GNMI_ADDR}" -insecure \
      -logtostderr set \
      -replace "${path}:$(cat "${file}")"
    return
  fi
  docker run --rm --network host -v "${ROOT}/deploy/payloads:/payloads:ro" \
    "${GNMIC_IMAGE}" \
    -a "${GNMI_ADDR}" --insecure --encoding json_ietf \
    set --replace-path "${path}" --replace-file "/payloads/$(basename "${file}")"
}

gnmi_delete() {
  local path="$1"
  if command -v gnmic >/dev/null 2>&1; then
    gnmic -a "${GNMI_ADDR}" --insecure --encoding json_ietf set --delete "${path}"
    return
  fi
  docker run --rm --network host \
    "${GNMIC_IMAGE}" \
    -a "${GNMI_ADDR}" --insecure --encoding json_ietf \
    set --delete "${path}"
}

ensure_adapter() {
  local so="${ADAPTER_SO:-${ROOT}/build/libsai_opi_adapter.so}"
  if [[ -f "${so}" ]]; then
    log "using adapter ${so}"
    return
  fi
  log "adapter .so not found at ${so} (syncd will log-only until you cmake-build it)"
}

clone_sai_challenger() {
  if [[ -d "${SAI_CHALLENGER_DIR}/.git" ]]; then
    log "SAI-Challenger already cloned at ${SAI_CHALLENGER_DIR}"
    return
  fi
  log "cloning OCP SAI-Challenger → ${SAI_CHALLENGER_DIR}"
  mkdir -p "$(dirname "${SAI_CHALLENGER_DIR}")"
  git clone --depth 1 "${SAI_CHALLENGER_REPO}" "${SAI_CHALLENGER_DIR}"
}

trap write_report EXIT

# --- compose -----------------------------------------------------------------
if [[ "${COMPOSE_UP}" -eq 1 ]]; then
  command -v docker >/dev/null || { echo "docker is required" >&2; exit 1; }
  ensure_adapter
  if [[ ! -f "${ROOT}/build/libsai_opi_adapter.so" ]]; then
    mkdir -p "${ROOT}/build"
    # Compose bind-mount fails if the source file is missing.
    : > "${ROOT}/build/libsai_opi_adapter.so"
    log "created placeholder ${ROOT}/build/libsai_opi_adapter.so"
  fi
  log "starting ${COMPOSE_FILE} ${PROFILE_ARGS[*]:-}"
  "${COMPOSE[@]}" "${PROFILE_ARGS[@]}" up --build -d
fi

log "waiting for gNMI ${GNMI_ADDR}"
wait_tcp "${GNMI_ADDR}" || { fail "gNMI not listening on ${GNMI_ADDR}"; exit 1; }
pass "sonic-gnmi listening on ${GNMI_ADDR}"
wait_tcp "${REDIS_HOST}:${REDIS_PORT}" || { fail "Redis APPL_DB not listening"; exit 1; }
pass "redis-appl-db listening on ${REDIS_HOST}:${REDIS_PORT}"
sleep 2

# --- gNMI DASH YANG/JSON (parent objects before children) -------------------
P="${ROOT}/deploy/payloads"
log "pushing DASH VNET / QoS / route-group / ACL-group"
gnmi_set "/sonic-dash/DASH_VNET/DASH_VNET_LIST[vnet_name=Vnet1]" "${P}/dash_vnet.json"
gnmi_set "/sonic-dash/DASH_QOS/DASH_QOS_LIST[qos_name=qos1]" "${P}/dash_qos.json"
gnmi_set "/sonic-dash/DASH_ROUTE_GROUP/DASH_ROUTE_GROUP_LIST[group_id=RouteGroup1]" "${P}/dash_route_group.json"
gnmi_set "/sonic-dash/DASH_ACL_GROUP/DASH_ACL_GROUP_LIST[group_id=acl-in-v4]" "${P}/dash_acl_group.json"
sleep 1
log "pushing DASH ENI / outbound route / ACL rule"
gnmi_set "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=F4939FEFC47E]" "${P}/dash_eni.json"
gnmi_set "/sonic-dash/DASH_ROUTE/DASH_ROUTE_LIST[group_id=RouteGroup1][prefix=10.0.0.0/16]" "${P}/dash_route.json"
gnmi_set "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=1]" "${P}/dash_acl_rule.json"
sleep 2

# --- hop 1: APPL_DB ----------------------------------------------------------
dump="$(redis_cli -n 0 KEYS 'DASH_*' || true)"
log "APPL_DB keys:"$'\n'"${dump}"
echo "${dump}" | grep -q 'DASH_VNET_TABLE:Vnet1' && pass "APPL_DB has DASH_VNET_TABLE:Vnet1" || fail "missing DASH_VNET_TABLE"
echo "${dump}" | grep -q 'DASH_ENI_TABLE' && pass "APPL_DB has DASH_ENI_TABLE" || fail "missing DASH_ENI_TABLE"
echo "${dump}" | grep -q 'DASH_ROUTE_TABLE' && pass "APPL_DB has DASH_ROUTE_TABLE" || fail "missing DASH_ROUTE_TABLE"
echo "${dump}" | grep -q 'DASH_ACL_RULE_TABLE' && pass "APPL_DB has DASH_ACL_RULE_TABLE" || fail "missing DASH_ACL_RULE_TABLE"

# --- hop 2: dashorch ---------------------------------------------------------
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q 'dashorch: APPL_DB change DASH_VNET' \
  && pass "dashorch consumed DASH_VNET" || fail "dashorch missed DASH_VNET"
echo "${orch_logs}" | grep -q 'create_vnet\|DASH_VNET_TABLE' \
  && pass "dashorch enqueued create_vnet" || fail "dashorch missed VNET create"
echo "${orch_logs}" | grep -q 'create_eni\|DASH_ENI_TABLE' \
  && pass "dashorch enqueued create_eni" || fail "dashorch missed ENI"
echo "${orch_logs}" | grep -q 'create_route' \
  && pass "dashorch enqueued create_route" || fail "dashorch missed route"
echo "${orch_logs}" | grep -q 'create_acl_rule' \
  && pass "dashorch enqueued create_acl_rule" || fail "dashorch missed ACL"

# --- hop 3: syncd + adapter --------------------------------------------------
syncd_logs="$(docker logs syncd 2>&1 || true)"
# Require real SAI call logs (sai_create_*), not bare work-queue "op=create_vnet"
# which still appears in log-only mode when the adapter .so failed to load.
echo "${syncd_logs}" | grep -Eq 'sai_create_vnet ' \
  && pass "syncd issued create_vnet" || fail "syncd missed create_vnet (adapter not calling SAI)"
echo "${syncd_logs}" | grep -Eq 'sai_create_eni ' \
  && pass "syncd issued sai_create_eni" || fail "syncd missed sai_create_eni"
echo "${syncd_logs}" | grep -Eq 'sai_create_outbound_routing_group ' \
  && pass "syncd issued create_route_group" || fail "syncd missed create_route_group"
echo "${syncd_logs}" | grep -Eq 'sai_create_outbound_routing_entry ' \
  && pass "syncd issued create_route" || fail "syncd missed create_route"
echo "${syncd_logs}" | grep -Eq 'sai_create_dash_acl_group ' \
  && pass "syncd issued create_acl_group" || fail "syncd missed create_acl_group"
echo "${syncd_logs}" | grep -Eq 'sai_create_dash_acl_rule ' \
  && pass "syncd issued create_acl_rule" || fail "syncd missed create_acl_rule"

if echo "${syncd_logs}" | grep -q 'dlopen failed\|cannot load\|missing shared libs\|WARNING.*ADAPTER'; then
  fail "adapter .so not loaded in syncd (see: docker logs syncd | head -40; ldd inside container)"
  log "hint: jammy-built adapter needs libprotobuf.so.23 in syncd (ubuntu:22.04 image)"
  log "      docker exec syncd ldd /usr/lib/libsai_opi_adapter.so | grep -E 'not found|grpc|protobuf'"
  log "      then: ./scripts/labctl.sh setup-sonic   # rebuilds syncd image"
fi
log "scoring 4×4 create RPCs from adapter logs"
score_rpc CreateVpc
score_rpc CreateSubnet
score_rpc CreateInterface
score_rpc CreateVnic
score_rpc CreateRouteTable
score_rpc CreateRoute
score_rpc CreateSecurityPolicy
score_rpc CreateSecurityRule

# --- hop 4: OPI gRPC ---------------------------------------------------------
if wait_tcp "${OPI_SERVER_ADDRESS}" 5; then
  pass "OPI gRPC reachable at ${OPI_SERVER_ADDRESS}"
  if command -v grpc_cli >/dev/null 2>&1; then
    grpc_cli ls --json_input --json_output "${OPI_SERVER_ADDRESS}" -l | head || true
  elif command -v grpcurl >/dev/null 2>&1; then
    grpcurl -plaintext "${OPI_SERVER_ADDRESS}" list || true
  else
    docker run --rm --network host "${GNMIC_IMAGE}" --help >/dev/null
    log "install grpc_cli/grpcurl to ListVnics; OPI TCP is open"
  fi
else
  fail "OPI gRPC ${OPI_SERVER_ADDRESS} not reachable (start DPU bridge or --profile mock-opi)"
fi

# --- SAI-Challenger ----------------------------------------------------------
clone_sai_challenger
export GNMI_ADDR REDIS_HOST REDIS_PORT OPI_SERVER_ADDRESS
if command -v python3 >/dev/null 2>&1; then
  python3 -m pip install -q pytest redis >/dev/null 2>&1 || true
  if python3 -m pytest -v "${ROOT}/deploy/sai-challenger/test_dash_opi_northbound.py"; then
    pass "SAI-Challenger northbound pytest"
  else
    fail "SAI-Challenger northbound pytest"
  fi
else
  fail "python3 not available for SAI-Challenger pytest"
fi

# --- 4×4 remove (children before parents; pytest already ran on live keys) --
ACL_RULE_PATH="/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=1]"
ROUTE_PATH="/sonic-dash/DASH_ROUTE/DASH_ROUTE_LIST[group_id=RouteGroup1][prefix=10.0.0.0/16]"
ENI_PATH="/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=F4939FEFC47E]"
ACL_GROUP_PATH="/sonic-dash/DASH_ACL_GROUP/DASH_ACL_GROUP_LIST[group_id=acl-in-v4]"
ROUTE_GROUP_PATH="/sonic-dash/DASH_ROUTE_GROUP/DASH_ROUTE_GROUP_LIST[group_id=RouteGroup1]"
VNET_PATH="/sonic-dash/DASH_VNET/DASH_VNET_LIST[vnet_name=Vnet1]"

log "deleting ACL rule via gNMI ${ACL_RULE_PATH}"
gnmi_delete "${ACL_RULE_PATH}"
sleep 1
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q '<- remove_acl_rule ' \
  && pass "dashorch enqueued remove_acl_rule" || fail "dashorch missed ACL rule delete"
syncd_logs="$(syncd_blob)"
echo "${syncd_logs}" | grep -Eq 'sai_remove_dash_acl_rule ' \
  && pass "syncd issued sai_remove_dash_acl_rule" || fail "syncd missed sai_remove_dash_acl_rule"
score_rpc DeleteSecurityRule

log "deleting outbound route via gNMI ${ROUTE_PATH}"
gnmi_delete "${ROUTE_PATH}"
sleep 1
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q '<- remove_route ' \
  && pass "dashorch enqueued remove_route" || fail "dashorch missed route delete"
syncd_logs="$(syncd_blob)"
echo "${syncd_logs}" | grep -Eq 'sai_remove_outbound_routing_entry ' \
  && pass "syncd issued sai_remove_outbound_routing_entry" || fail "syncd missed sai_remove_outbound_routing_entry"
score_rpc DeleteRoute

log "deleting DASH ENI via gNMI ${ENI_PATH}"
gnmi_delete "${ENI_PATH}"
sleep 2
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q 'remove_eni\|APPL_DB delete DASH_ENI_TABLE' \
  && pass "dashorch enqueued remove_eni" || fail "dashorch missed ENI delete"
syncd_logs="$(syncd_blob)"
echo "${syncd_logs}" | grep -Eq 'sai_remove_eni ' \
  && pass "syncd issued sai_remove_eni" || fail "syncd missed sai_remove_eni"
score_rpc DeleteVnic
score_rpc DeleteInterface
dump_after="$(redis_cli -n 0 KEYS 'DASH_ENI_TABLE*' || true)"
echo "${dump_after}" | grep -q 'DASH_ENI_TABLE' \
  && fail "DASH_ENI_TABLE still present after gNMI delete" \
  || pass "APPL_DB DASH_ENI_TABLE removed"

log "deleting ACL group via gNMI ${ACL_GROUP_PATH}"
gnmi_delete "${ACL_GROUP_PATH}"
sleep 1
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q '<- remove_acl_group ' \
  && pass "dashorch enqueued remove_acl_group" || fail "dashorch missed ACL group delete"
syncd_logs="$(syncd_blob)"
echo "${syncd_logs}" | grep -Eq 'sai_remove_dash_acl_group ' \
  && pass "syncd issued sai_remove_dash_acl_group" || fail "syncd missed sai_remove_dash_acl_group"
score_rpc DeleteSecurityPolicy

log "deleting route group via gNMI ${ROUTE_GROUP_PATH}"
gnmi_delete "${ROUTE_GROUP_PATH}"
sleep 1
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q '<- remove_route_group ' \
  && pass "dashorch enqueued remove_route_group" || fail "dashorch missed route group delete"
syncd_logs="$(syncd_blob)"
echo "${syncd_logs}" | grep -Eq 'sai_remove_outbound_routing_group ' \
  && pass "syncd issued sai_remove_outbound_routing_group" || fail "syncd missed sai_remove_outbound_routing_group"
score_rpc DeleteRouteTable

log "deleting VNET via gNMI ${VNET_PATH}"
gnmi_delete "${VNET_PATH}"
sleep 2
orch_logs="$(docker logs dashorch 2>&1 || true)"
echo "${orch_logs}" | grep -q '<- remove_vnet ' \
  && pass "dashorch enqueued remove_vnet" || fail "dashorch missed VNET delete"
syncd_logs="$(syncd_blob)"
echo "${syncd_logs}" | grep -Eq 'sai_remove_vnet ' \
  && pass "syncd issued sai_remove_vnet" || fail "syncd missed sai_remove_vnet"
score_rpc DeleteSubnet
score_rpc DeleteVpc
dump_vnet="$(redis_cli -n 0 KEYS 'DASH_VNET_TABLE*' || true)"
echo "${dump_vnet}" | grep -q 'DASH_VNET_TABLE' \
  && fail "DASH_VNET_TABLE still present after gNMI delete" \
  || pass "APPL_DB DASH_VNET_TABLE removed"

if [[ -x "${SAI_CHALLENGER_DIR}/exec.sh" ]]; then
  log "OCP SAI-Challenger clone is at ${SAI_CHALLENGER_DIR}"
  log "Dataplane saivs: (cd ${SAI_CHALLENGER_DIR} && ./build.sh -a trident2 -t saivs && ./run.sh -a trident2 -t saivs)"
  log "This lab uses the gNMI northbound path; saivs is optional for packet tests."
fi

write_report
log "results: ${PASS} passed, ${FAIL} failed"
[[ "${FAIL}" -eq 0 ]]
