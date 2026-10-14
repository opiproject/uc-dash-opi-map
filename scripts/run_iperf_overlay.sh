#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Two ENIs in the same VNET, then iperf3 across them.
#
#   ./scripts/labctl.sh setup-sonic   # stack up, objects empty or after run-sonic
#   ./scripts/labctl.sh run-iperf     # this script
#
# Does not delete. DATAPATH=ovs: CloudInfra programs same-VNI L2 on br-opi
# so two host VFs can talk without a remote VTEP. DOCA still lacks that L2.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GNMI_ADDR="${GNMI_ADDR:-127.0.0.1:8080}"
GNMIC_IMAGE="${GNMIC_IMAGE:-ghcr.io/openconfig/gnmic:0.38.2}"
P="${ROOT}/deploy/payloads"
TENANT_IP_A="${TENANT_IP_A:-10.0.0.1}"
TENANT_IP_B="${TENANT_IP_B:-10.0.0.2}"
IPERF_SECONDS="${IPERF_SECONDS:-10}"
ENI_A="${ENI_A:-F4939FEFC47E}"
ENI_B="${ENI_B:-001A2B3C4D5F}"
MAC_A="${MAC_A:-f4:93:9f:ef:c4:7e}"
MAC_B="${MAC_B:-00:1a:2b:3c:4d:5f}"
HOST_VF_A="${HOST_VF_A:-}"
HOST_VF_B="${HOST_VF_B:-}"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }

gnmi_set() {
  local path="$1" file="$2"
  if command -v gnmic >/dev/null 2>&1; then
    gnmic -a "${GNMI_ADDR}" --insecure --encoding json_ietf \
      set --replace-path "${path}" --replace-file "${file}"
    return
  fi
  docker run --rm --network host -v "${ROOT}/deploy/payloads:/payloads:ro" \
    "${GNMIC_IMAGE}" \
    -a "${GNMI_ADDR}" --insecure --encoding json_ietf \
    set --replace-path "${path}" --replace-file "/payloads/$(basename "${file}")"
}

wait_tcp() {
  local spec="$1" n="${2:-30}"
  local host="${spec%:*}" port="${spec##*:}" i
  for ((i = 0; i < n; i++)); do
    if command -v nc >/dev/null 2>&1 && nc -z "${host}" "${port}" 2>/dev/null; then
      return 0
    fi
    python3 -c "import socket; s=socket.create_connection(('${host}',${port}),1); s.close()" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

log "run-iperf — two ENIs in Vnet1 (${ENI_A} + ${ENI_B}) then iperf3 ${TENANT_IP_B}→${TENANT_IP_A}"
wait_tcp "${GNMI_ADDR}" 15 || { log "gNMI ${GNMI_ADDR} not up; run: ./scripts/labctl.sh setup-sonic"; exit 1; }

log "pushing VNET / QoS / route-group / ACL-group (same as run-sonic)"
gnmi_set "/sonic-dash/DASH_VNET/DASH_VNET_LIST[vnet_name=Vnet1]" "${P}/dash_vnet.json"
gnmi_set "/sonic-dash/DASH_QOS/DASH_QOS_LIST[qos_name=qos1]" "${P}/dash_qos.json"
gnmi_set "/sonic-dash/DASH_ROUTE_GROUP/DASH_ROUTE_GROUP_LIST[group_id=RouteGroup1]" "${P}/dash_route_group.json"
gnmi_set "/sonic-dash/DASH_ACL_GROUP/DASH_ACL_GROUP_LIST[group_id=acl-in-v4]" "${P}/dash_acl_group.json"
sleep 1

log "pushing ENI A (${ENI_A}) and ENI B (${ENI_B}) on Vnet1"
gnmi_set "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=${ENI_A}]" "${P}/dash_eni.json"
gnmi_set "/sonic-dash/DASH_ENI/DASH_ENI_LIST[eni=${ENI_B}]" "${P}/dash_eni_b.json"
log "pushing route 10.0.0.0/16 and ACL permit tcp/80 + tcp/5201 (iperf3)"
gnmi_set "/sonic-dash/DASH_ROUTE/DASH_ROUTE_LIST[group_id=RouteGroup1][prefix=10.0.0.0/16]" "${P}/dash_route.json"
gnmi_set "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=1]" "${P}/dash_acl_rule.json"
gnmi_set "/sonic-dash/DASH_ACL_RULE/DASH_ACL_RULE_LIST[group_id=acl-in-v4][rule_num=2]" "${P}/dash_acl_rule_iperf.json"
sleep 3

count="$(docker logs syncd 2>/dev/null | grep -c 'sai_create_eni ' || true)"
if [[ "${count}" -ge 2 ]]; then
  log "PASS: syncd issued sai_create_eni ${count} times"
else
  log "WARN: expected two sai_create_eni lines (got ${count}; docker logs syncd)"
fi
if docker logs syncd 2>/dev/null | grep -q 'CreateVnic'; then
  log "PASS: CloudInfra CreateVnic in syncd logs"
fi

log "objects left in place (no gNMI delete). Check SoC:"
log "  ssh \$SSH_TARGET ovs-ofctl -O OpenFlow13 dump-flows br-opi"
log "  look for priority=220 same-VNI L2 (dl_dst=peer MAC) and cookie overlay"

if [[ -z "${HOST_VF_A}" || -z "${HOST_VF_B}" ]]; then
  cat <<EOF
${0##*/}: HOST_VF_A / HOST_VF_B unset — programmed two ENIs; run iperf on the host VFs:

  # find VFs: ip -br link
  sudo ip link set \$HOST_VF_A address ${MAC_A}
  sudo ip link set \$HOST_VF_B address ${MAC_B}
  sudo ip addr add ${TENANT_IP_A}/16 dev \$HOST_VF_A
  sudo ip addr add ${TENANT_IP_B}/16 dev \$HOST_VF_B
  sudo ip link set \$HOST_VF_A up
  sudo ip link set \$HOST_VF_B up
  sudo iperf3 -s -B ${TENANT_IP_A} -1 &
  sudo iperf3 -c ${TENANT_IP_A} -B ${TENANT_IP_B} -t ${IPERF_SECONDS}

Set HOST_VF_A / HOST_VF_B in config/lab.env to have labctl run-iperf do this.
DATAPATH=ovs is required for local ENI switching. Then: ./scripts/labctl.sh cleanup
EOF
  exit 0
fi

if [[ ! -d "/sys/class/net/${HOST_VF_A}" || ! -d "/sys/class/net/${HOST_VF_B}" ]]; then
  log "error: HOST_VF_A=${HOST_VF_A} HOST_VF_B=${HOST_VF_B} — netdev missing on this host"
  exit 1
fi
if ! command -v iperf3 >/dev/null 2>&1; then
  log "error: install iperf3 (sudo apt-get install -y iperf3)"
  exit 1
fi

sudo_ip() { sudo ip "$@"; }

log "configuring ${HOST_VF_A}=${TENANT_IP_A} mac=${MAC_A}  ${HOST_VF_B}=${TENANT_IP_B} mac=${MAC_B}"
sudo_ip link set "${HOST_VF_A}" address "${MAC_A}" || true
sudo_ip link set "${HOST_VF_B}" address "${MAC_B}" || true
sudo_ip addr replace "${TENANT_IP_A}/16" dev "${HOST_VF_A}"
sudo_ip addr replace "${TENANT_IP_B}/16" dev "${HOST_VF_B}"
sudo_ip link set "${HOST_VF_A}" up
sudo_ip link set "${HOST_VF_B}" up
sudo sysctl -w "net.ipv4.conf.${HOST_VF_A}.rp_filter=0" >/dev/null
sudo sysctl -w "net.ipv4.conf.${HOST_VF_B}.rp_filter=0" >/dev/null

log "iperf3 -s -B ${TENANT_IP_A}  then  iperf3 -c ${TENANT_IP_A} -B ${TENANT_IP_B}"
iperf3 -s -B "${TENANT_IP_A}" -1 >/tmp/dash-opi-iperf-s.log 2>&1 &
srv=$!
sleep 1
if iperf3 -c "${TENANT_IP_A}" -B "${TENANT_IP_B}" -t "${IPERF_SECONDS}" -P 1; then
  log "PASS: iperf3 ${TENANT_IP_B} → ${TENANT_IP_A} through two ENIs in Vnet1"
else
  log "FAIL: iperf3 (see /tmp/dash-opi-iperf-s.log). DATAPATH=ovs? dump-flows br-opi on the DPU."
  wait "${srv}" 2>/dev/null || true
  exit 1
fi
wait "${srv}" 2>/dev/null || true
log "leave ENIs up for more tests, or: ./scripts/labctl.sh cleanup"
