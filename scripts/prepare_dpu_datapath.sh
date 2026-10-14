#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Bring up the chosen SoC packet backend BEFORE CloudInfra starts.
# setup-dpu / start_cloudinfra_dpu.sh call this on the DPU.
#
# Reads config/lab.env when present (DPU prereqs). CLI flags override.
#
#   sudo ./scripts/prepare_dpu_datapath.sh --datapath ovs
#   sudo ./scripts/prepare_dpu_datapath.sh --datapath doca
#   sudo ./scripts/prepare_dpu_datapath.sh --datapath auto
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -f "${ROOT}/config/lab.env" ]]; then
  set -a
  # strip CR so a Windows-edited lab.env still sources on the DPU
  # shellcheck disable=SC1090
  source <(sed 's/\r$//' "${ROOT}/config/lab.env")
  set +a
fi

DATAPATH="${OPI_DATAPATH:-${DATAPATH:-ovs}}"
SOCK="${OPI_DOCA_SOCKET:-${DPU_DOCA_SOCKET:-/var/run/opi-doca.sock}}"
INSTALL="${DPU_INSTALL_PACKAGES:-1}"
UPLINK="${DPU_UPLINK:-${OPI_UPLINK:-p0}}"
REPS="${DPU_HOST_REPRESENTORS:-pf0hpf pf0vf0 pf0vf1}"
HUGE="${DPU_HUGEPAGES_2MB:-1024}"
SWITCHDEV="${DPU_SWITCHDEV:-0}"
STEAL="${DPU_STEAL_REPS:-0}"
DOCA_STUB="${DPU_DOCA_STUB:-0}"
DRY=0
RESTORE_OVS=0
STOLEN_FILE="${ROOT}/config/stolen-ovsbr1.ports"

usage() {
  cat <<'EOF'
Usage: prepare_dpu_datapath.sh --datapath ovs|doca|auto [--dry-run]
       prepare_dpu_datapath.sh --restore-ovsbr1

  ovs   Install/start Open vSwitch. Fail if it cannot run.
        Does not create br-opi (CloudInfra still does that).
        Does not touch vendor ovsbr1 / p0 / pf0hpf unless DPU_STEAL_REPS=1.
  doca  Hugepages + dpu_doca_flowd (DOCA SDK unless DPU_DOCA_STUB=1).
        Do not start OVS for br-opi — eSwitch and br-opi must not share representors.
  auto  If the DOCA socket is already up, use it; else start OVS.
  --restore-ovsbr1
        Put ports stolen from ovsbr1 back (undo DPU_STEAL_REPS=1).

Must run on the DPU SoC. Site knobs: config/lab.env (copied by setup-dpu).
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --datapath) DATAPATH="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --restore-ovsbr1) RESTORE_OVS=1; shift ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

restore_stolen_ovsbr1() {
  if [[ ! -f "${STOLEN_FILE}" ]]; then
    log "no stolen-port list (${STOLEN_FILE}); nothing to put back on ovsbr1"
    return 0
  fi
  have ovs-vsctl || die "ovs-vsctl missing; cannot restore ovsbr1"
  ovs-vsctl br-exists ovsbr1 2>/dev/null || die "ovsbr1 does not exist; cannot restore ports"
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: would restore $(tr '\n' ' ' <"${STOLEN_FILE}") to ovsbr1"
    return 0
  fi
  local r
  while read -r r; do
    [[ -n "${r}" ]] || continue
    [[ -d "/sys/class/net/${r}" ]] || { log "WARN: ${r} missing; skip add-port ovsbr1"; continue; }
    log "restore: ovs-vsctl --may-exist add-port ovsbr1 ${r}"
    ovs-vsctl --may-exist add-port ovsbr1 "${r}" || log "WARN: could not add-port ovsbr1 ${r}"
    ip link set "${r}" up 2>/dev/null || true
  done <"${STOLEN_FILE}"
  rm -f "${STOLEN_FILE}"
  log "ovsbr1 restored; vendor OVS owns those ports again"
}

DATAPATH="$(printf '%s' "${DATAPATH}" | tr '[:upper:]' '[:lower:]')"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

if [[ "${RESTORE_OVS}" -eq 1 ]]; then
  restore_stolen_ovsbr1
  exit 0
fi

apt_install() {
  local pkgs=("$@")
  [[ "${INSTALL}" == "1" ]] || die "missing ${pkgs[*]}; set DPU_INSTALL_PACKAGES=1 in config/lab.env or apt-get install them"
  have apt-get || die "apt-get not available; install ${pkgs[*]} by hand"
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: apt-get install ${pkgs[*]}"
    return 0
  fi
  log "apt-get install ${pkgs[*]}"
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${pkgs[@]}"
}

ensure_pkg_cmd() {
  local cmd="$1"
  shift
  have "${cmd}" && return 0
  apt_install "$@"
  have "${cmd}" || die "still missing ${cmd} after apt-get install $*"
}

link_up() {
  local d="$1"
  [[ -n "${d}" ]] || return 0
  [[ -d "/sys/class/net/${d}" ]] || return 0
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: ip link set ${d} up"
    return 0
  fi
  ip link set "${d}" up 2>/dev/null || log "WARN: could not up ${d}"
}

ensure_interfaces() {
  have ip || apt_install iproute2
  log "bringing up uplink=${UPLINK} representors=${REPS}"
  link_up "${UPLINK}"
  local r
  # shellcheck disable=SC2086
  for r in ${REPS}; do
    link_up "${r}"
  done
}

ensure_hugepages() {
  local want="${HUGE}"
  [[ "${want}" =~ ^[0-9]+$ ]] || want=1024
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: hugepages-2048kB=${want}"
    return 0
  fi
  local hp="/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
  if [[ -w "${hp}" ]]; then
    local cur
    cur="$(cat "${hp}" 2>/dev/null || echo 0)"
    if [[ "${cur}" -lt "${want}" ]]; then
      echo "${want}" >"${hp}" || log "WARN: could not raise hugepages to ${want}"
    fi
  else
    log "WARN: ${hp} not writable; DOCA/DPDK may fail"
  fi
  mkdir -p /mnt/huge
  mountpoint -q /mnt/huge || mount -t hugetlbfs -o pagesize=2M nodev /mnt/huge || true
}

maybe_switchdev() {
  [[ "${SWITCHDEV}" == "1" ]] || return 0
  log "DPU_SWITCHDEV=1: attempting mlx5 eSwitch switchdev"
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: skip switchdev"
    return 0
  fi
  have devlink || apt_install iproute2
  local changed=0
  local pci
  for pci in /sys/bus/pci/devices/*; do
    local drv
    drv="$(basename "$(readlink "${pci}/driver" 2>/dev/null || true)" || true)"
    [[ "${drv}" == "mlx5_core" ]] || continue
    local bdf
    bdf="$(basename "${pci}")"
    if have devlink && devlink dev eswitch show "pci/${bdf}" >/dev/null 2>&1; then
      local mode
      mode="$(devlink dev eswitch show "pci/${bdf}" 2>/dev/null || true)"
      if printf '%s' "${mode}" | grep -q 'legacy'; then
        log "switchdev: pci/${bdf} legacy → switchdev"
        devlink dev eswitch set "pci/${bdf}" mode switchdev \
          || log "WARN: switchdev set failed for pci/${bdf} (may need host-side mlxconfig + reboot)"
        changed=1
      else
        log "switchdev: pci/${bdf} already not legacy"
      fi
    fi
  done
  [[ "${changed}" -eq 1 ]] && log "WARN: eSwitch mode change may need a representor re-probe; re-run setup-dpu if pf0hpf is missing"
}

ovsbr1_held() {
  have ovs-vsctl || return 0
  ovs-vsctl br-exists ovsbr1 2>/dev/null || return 0
  local r held=""
  # shellcheck disable=SC2086
  for r in ${UPLINK} ${REPS}; do
    [[ -n "${r}" ]] || continue
    if ovs-vsctl list-ports ovsbr1 2>/dev/null | grep -qx "${r}"; then
      held="${held} ${r}"
    fi
  done
  printf '%s' "${held}"
}

maybe_steal_reps() {
  [[ "${STEAL}" == "1" ]] || return 0
  have ovs-vsctl || return 0
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: skip steal representors from ovsbr1"
    return 0
  fi
  ovs-vsctl br-exists ovsbr1 2>/dev/null || return 0
  local r
  # shellcheck disable=SC2086
  for r in ${REPS} ${UPLINK}; do
    [[ -n "${r}" ]] || continue
    if ovs-vsctl list-ports ovsbr1 2>/dev/null | grep -qx "${r}"; then
      log "DPU_STEAL_REPS=1: ovs-vsctl del-port ovsbr1 ${r}"
      mkdir -p "$(dirname "${STOLEN_FILE}")"
      grep -qx "${r}" "${STOLEN_FILE}" 2>/dev/null || echo "${r}" >>"${STOLEN_FILE}"
      ovs-vsctl del-port ovsbr1 "${r}" || log "WARN: could not del-port ovsbr1 ${r}"
    fi
  done
}

# DOCA switch and vendor OVS cannot both own p0 / representors.
doca_release_eswitch() {
  local held
  held="$(ovsbr1_held)"
  [[ -n "${held}" ]] || return 0
  if [[ "${STEAL}" == "1" ]]; then
    maybe_steal_reps
    held="$(ovsbr1_held)"
    [[ -z "${held}" ]] || die "DPU_STEAL_REPS=1 but ovsbr1 still holds:${held}"
    return 0
  fi
  die "datapath=doca: vendor ovsbr1 still holds:${held}. Set DPU_STEAL_REPS=1 in config/lab.env and re-run setup-dpu (DOCA eSwitch cannot share those ports with OVS)."
}

ovs_running() {
  pidof ovs-vswitchd >/dev/null 2>&1
}

start_ovs_service() {
  if have systemctl; then
    systemctl start openvswitch-switch 2>/dev/null && return 0
    systemctl start openvswitch.service 2>/dev/null && return 0
  fi
  if have service; then
    service openvswitch-switch start 2>/dev/null && return 0
    service openvswitch start 2>/dev/null && return 0
  fi
  return 1
}

ensure_ovs() {
  log "datapath=ovs: installing/starting Open vSwitch"
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: skip starting ovs-vswitchd"
    return 0
  fi
  ensure_pkg_cmd ovs-vsctl openvswitch-switch
  ensure_pkg_cmd ovs-ofctl openvswitch-switch
  if ! ovs_running; then
    log "ovs-vswitchd not running; starting openvswitch"
    start_ovs_service || die "could not start openvswitch (systemctl start openvswitch-switch)"
    sleep 0.5
  fi
  ovs_running || die "ovs-vswitchd still not running after start"
  if ! ovs-vsctl show >/dev/null 2>&1; then
    die "ovs-vsctl show failed (ovsdb-server down?)"
  fi
  maybe_steal_reps
  log "OVS ready (ovs-vswitchd up; br-opi is created later by CloudInfra)"
}

stop_doca_sidecar() {
  if [[ -f /var/run/dpu-doca-flowd.pid ]]; then
    kill "$(cat /var/run/dpu-doca-flowd.pid)" >/dev/null 2>&1 || true
  fi
  pkill -f '/dpu_doca_flowd( |$)' >/dev/null 2>&1 || true
  rm -f "${SOCK}" /var/run/dpu-doca-flowd.pid
}

ensure_doca_sdk() {
  [[ "${DOCA_STUB}" == "1" ]] && return 0
  export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}:/opt/mellanox/doca/lib/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/doca/lib64/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/dpdk/lib/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig"
  if pkg-config --exists doca-flow 2>/dev/null || pkg-config --exists libdoca-sdk-flow 2>/dev/null; then
    log "pkg-config DOCA Flow OK ($(pkg-config --modversion doca-flow 2>/dev/null || pkg-config --modversion libdoca-sdk-flow 2>/dev/null))"
    return 0
  fi
  local pcs=""
  pcs="$(find /opt/mellanox/doca /opt/mellanox/dpdk /usr/lib -name '*doca*.pc' -o -name '*flow*.pc' 2>/dev/null | head -n 20 | tr '\n' ' ' || true)"
  die "DOCA SDK pkg-config failed (looked for doca-flow / libdoca-sdk-flow). pc files: ${pcs:-none}. Set DPU_DOCA_STUB=1 or DATAPATH=ovs"
}

ensure_doca() {
  log "datapath=doca: hugepages + dpu_doca_flowd (${SOCK})"
  if [[ "${DRY}" -eq 1 ]]; then
    log "dry-run: skip starting dpu_doca_flowd"
    return 0
  fi
  ensure_pkg_cmd gcc build-essential pkg-config
  ensure_hugepages
  ensure_doca_sdk
  doca_release_eswitch
  local helper="${ROOT}/scripts/start_doca_flowd.sh"
  [[ -f "${helper}" ]] || die "missing ${helper}"
  chmod +x "${helper}"
  local args=(--socket "${SOCK}")
  [[ "${DOCA_STUB}" == "1" ]] && args+=(--stub)
  OPI_DOCA_SOCKET="${SOCK}" DPU_HUGEPAGES_2MB="${HUGE}" \
    bash "${helper}" "${args[@]}" || die "dpu_doca_flowd failed; install DOCA SDK or set DPU_DOCA_STUB=1"
  [[ -S "${SOCK}" ]] || die "DOCA socket ${SOCK} missing after start"
  log "DOCA sidecar ready (${SOCK})"
}

doca_socket_up() {
  [[ -S "${SOCK}" ]]
}

maybe_switchdev
ensure_interfaces

case "${DATAPATH}" in
  ovs)
    stop_doca_sidecar
    ensure_ovs
    ;;
  doca)
    ensure_doca
    ;;
  auto)
    if doca_socket_up; then
      log "datapath=auto: ${SOCK} already up — CloudInfra will use DOCA"
    else
      log "datapath=auto: no DOCA socket; starting OVS"
      ensure_ovs
    fi
    ;;
  *)
    die "unknown --datapath ${DATAPATH} (ovs|doca|auto)"
    ;;
esac
