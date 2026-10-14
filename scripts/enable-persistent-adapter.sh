#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Re-bind SONiC's libsai.so to libsai_opi_adapter.so whenever the syncd
# container is created, started, restarted, or replaced from a vendor image.
#
# Vendor images restore proprietary libsai.so on recreate. /etc/sonic is
# bind-mounted from the host and survives; the SAI .so inside the image does
# not. This hook keeps a canonical copy on the host and reapplies it on every
# Docker *start* event — without `docker restart` (that would loop).
#
#   sudo ./scripts/enable-persistent-adapter.sh \
#       --lib build/libsai_opi_adapter.so \
#       --container syncd
#
#   sudo ./scripts/enable-persistent-adapter.sh --rebind-now
#   sudo ./scripts/enable-persistent-adapter.sh --uninstall
set -euo pipefail

STATE_DIR="${SAI_OPI_STATE_DIR:-/var/lib/sai-opi}"
LIBEXEC_DIR="${SAI_OPI_LIBEXEC_DIR:-/usr/local/libexec/sai-opi}"
UNIT_PATH="${SAI_OPI_UNIT_PATH:-/etc/systemd/system/sai-opi-persist.service}"
SONIC_HOOK_DIR="${SAI_OPI_SONIC_HOOK_DIR:-/etc/sonic/sai-opi}"
ADAPTER_SO=""
SYNCD_CONTAINER="${SYNCD_CONTAINER:-syncd}"
ACTION="install"

usage() {
  cat <<'EOF'
Usage: enable-persistent-adapter.sh [options]

  --lib PATH         Canonical libsai_opi_adapter.so (copied to /var/lib/sai-opi)
  --container NAME   Docker name to watch (default: syncd)
  --rebind-now       Apply the bind once and exit (watcher already installed)
  --uninstall        Stop the watcher and remove systemd unit / libexec files
  --watch-foreground Run the docker-events loop in this terminal (no systemd)

Host layout after install:
  /var/lib/sai-opi/libsai_opi_adapter.so   golden copy
  /usr/local/libexec/sai-opi/rebind.sh     docker cp + symlink + supervisorctl
  /usr/local/libexec/sai-opi/watch.sh      docker events → rebind.sh
  /etc/systemd/system/sai-opi-persist.service
  /etc/sonic/sai-opi/                    also bind-mounted into the container
EOF
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --lib) ADAPTER_SO="$2"; shift 2 ;;
    --container) SYNCD_CONTAINER="$2"; shift 2 ;;
    --rebind-now) ACTION="rebind"; shift ;;
    --uninstall) ACTION="uninstall"; shift ;;
    --watch-foreground) ACTION="watch"; shift ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }

require_root() {
  [[ "${EUID}" -eq 0 ]] || die "re-run as root"
}

resolve_adapter() {
  if [[ -n "${ADAPTER_SO}" && -f "${ADAPTER_SO}" ]]; then
    return 0
  fi
  local cand
  for cand in \
      "${STATE_DIR}/libsai_opi_adapter.so" \
      /usr/lib/libsai_opi_adapter.so \
      /usr/lib/"$(uname -m)"-linux-gnu/libsai_opi_adapter.so \
      ./build/libsai_opi_adapter.so \
      ./build/lib/libsai_opi_adapter.so; do
    if [[ -f "${cand}" ]]; then
      ADAPTER_SO="${cand}"
      return 0
    fi
  done
  return 1
}

write_rebind() {
  cat > "${LIBEXEC_DIR}/rebind.sh" <<'REBIND'
#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# In-place rebind. Never `docker restart` — that re-enters the start watcher.
set -euo pipefail

STATE_DIR="${SAI_OPI_STATE_DIR:-/var/lib/sai-opi}"
# shellcheck source=/dev/null
[[ -f "${STATE_DIR}/persist.env" ]] && . "${STATE_DIR}/persist.env"
CONTAINER="${SYNCD_CONTAINER:-syncd}"
ADAPTER="${STATE_DIR}/libsai_opi_adapter.so"
LOCK="${STATE_DIR}/rebind.lock"
LOG="${STATE_DIR}/rebind.log"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "${LOG}"; }

[[ -f "${ADAPTER}" ]] || { log "missing ${ADAPTER}"; exit 1; }
command -v docker >/dev/null 2>&1 || { log "docker not found"; exit 1; }
docker inspect "${CONTAINER}" >/dev/null 2>&1 || { log "container ${CONTAINER} not found"; exit 0; }

exec 9>"${LOCK}"
flock -n 9 || { log "rebind already running"; exit 0; }

# Wait until docker exec works (entrypoint still starting).
ready=0
for _ in $(seq 1 40); do
  if docker inspect -f '{{.State.Running}}' "${CONTAINER}" 2>/dev/null | grep -qx true \
      && docker exec "${CONTAINER}" true >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.5
done
[[ "${ready}" -eq 1 ]] || { log "timeout waiting for ${CONTAINER} exec"; exit 1; }

log "rebind libsai.so in ${CONTAINER}"
docker cp "${ADAPTER}" "${CONTAINER}:/tmp/libsai_opi_adapter.so"

docker exec -i "${CONTAINER}" bash -s <<'INSIDE'
set -euo pipefail
for d in /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib/"$(uname -m)"-linux-gnu; do
  [[ -d "$d" ]] || continue
  if [[ -e "$d/libsai.so" && ! -e "$d/libsai.so.vendor.bak" ]]; then
    cp -a "$d/libsai.so" "$d/libsai.so.vendor.bak" || true
  fi
  install -m 0755 /tmp/libsai_opi_adapter.so "$d/libsai_opi_adapter.so"
  ln -sfn libsai_opi_adapter.so "$d/libsai.so"
done
mkdir -p /etc/sai.d /etc/sonic
ldconfig || true

# Reload the SAI .so without restarting the container (avoids docker-event loops).
if command -v supervisorctl >/dev/null 2>&1; then
  supervisorctl restart syncd >/dev/null 2>&1 \
    || supervisorctl restart docker-syncd >/dev/null 2>&1 \
    || true
fi
INSIDE

log "rebind complete (${CONTAINER})"
REBIND
  chmod 0755 "${LIBEXEC_DIR}/rebind.sh"
}

write_watch() {
  cat > "${LIBEXEC_DIR}/watch.sh" <<'WATCH'
#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# docker events → rebind. Matches container name from persist.env.
set -euo pipefail

STATE_DIR="${SAI_OPI_STATE_DIR:-/var/lib/sai-opi}"
LIBEXEC_DIR="${SAI_OPI_LIBEXEC_DIR:-/usr/local/libexec/sai-opi}"
# shellcheck source=/dev/null
[[ -f "${STATE_DIR}/persist.env" ]] && . "${STATE_DIR}/persist.env"
CONTAINER="${SYNCD_CONTAINER:-syncd}"
REBIND="${LIBEXEC_DIR}/rebind.sh"
LOG="${STATE_DIR}/watch.log"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "${LOG}"; }

command -v docker >/dev/null 2>&1 || { log "docker not found"; exit 1; }
[[ -x "${REBIND}" ]] || { log "missing ${REBIND}"; exit 1; }

match_container() {
  local name="$1"
  [[ -z "${name}" ]] && return 1
  [[ "${name}" == "${CONTAINER}" ]] && return 0
  # SONiC sometimes prefixes (e.g. sonic-syncd, docker-syncd-brcm).
  [[ "${name}" == *"${CONTAINER}"* ]] && return 0
  return 1
}

log "watching docker start events for container name '${CONTAINER}'"
# Apply once in case syncd is already up with vendor libsai.so.
"${REBIND}" || true

docker events --filter type=container --filter event=start \
    --format '{{.Actor.Attributes.name}}' | while IFS= read -r name; do
  if match_container "${name}"; then
    log "start event name=${name}"
    "${REBIND}" || log "rebind failed for ${name}"
  fi
done
WATCH
  chmod 0755 "${LIBEXEC_DIR}/watch.sh"
}

write_container_hook() {
  # Lives on the host under /etc/sonic (bind-mounted into syncd). Operators
  # can also docker exec bash /etc/sonic/sai-opi/container-hook.sh.
  cat > "${SONIC_HOOK_DIR}/container-hook.sh" <<'HOOK'
#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Optional in-container helper ( /etc/sonic is bind-mounted from the host ).
# The host watcher is the supported path; this is for manual recovery.
set -euo pipefail
echo "Run this from the SONiC host, not as the container entrypoint:"
echo "  sudo /usr/local/libexec/sai-opi/rebind.sh"
exit 1
HOOK
  chmod 0755 "${SONIC_HOOK_DIR}/container-hook.sh"
}

write_unit() {
  cat > "${UNIT_PATH}" <<EOF
[Unit]
Description=Re-bind libsai.so to libsai_opi_adapter.so after syncd container start
Documentation=file://${STATE_DIR}/README
After=docker.service
Wants=docker.service

[Service]
Type=simple
EnvironmentFile=-${STATE_DIR}/persist.env
Environment=SAI_OPI_STATE_DIR=${STATE_DIR}
ExecStart=${LIBEXEC_DIR}/watch.sh
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
}

install_hook() {
  require_root
  command -v docker >/dev/null 2>&1 || die "docker is required on the SONiC host"
  resolve_adapter || die "adapter .so not found; pass --lib"

  mkdir -p "${STATE_DIR}" "${LIBEXEC_DIR}" "${SONIC_HOOK_DIR}"
  install -m 0755 "${ADAPTER_SO}" "${STATE_DIR}/libsai_opi_adapter.so"
  cat > "${STATE_DIR}/persist.env" <<EOF
# Generated by enable-persistent-adapter.sh
SYNCD_CONTAINER=${SYNCD_CONTAINER}
SAI_OPI_STATE_DIR=${STATE_DIR}
SAI_OPI_LIBEXEC_DIR=${LIBEXEC_DIR}
EOF
  chmod 0644 "${STATE_DIR}/persist.env"

  write_rebind
  write_watch
  write_container_hook
  write_unit

  cat > "${STATE_DIR}/README" <<EOF
Canonical adapter: ${STATE_DIR}/libsai_opi_adapter.so
Watched container: ${SYNCD_CONTAINER}
Rebind (no docker restart): ${LIBEXEC_DIR}/rebind.sh
EOF

  if command -v systemctl >/dev/null 2>&1 && [[ -d /run/systemd/system ]]; then
    systemctl daemon-reload
    systemctl enable --now sai-opi-persist.service
    log "systemd unit sai-opi-persist.service enabled"
  else
    log "systemd not available; start the watcher with:"
    log "  ${LIBEXEC_DIR}/watch.sh"
  fi

  "${LIBEXEC_DIR}/rebind.sh" || log "initial rebind skipped (container may be down)"
  log "persistent adapter enabled for container '${SYNCD_CONTAINER}'"
  log "golden copy: ${STATE_DIR}/libsai_opi_adapter.so"
}

uninstall_hook() {
  require_root
  if command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now sai-opi-persist.service 2>/dev/null || true
    systemctl daemon-reload || true
  fi
  rm -f "${UNIT_PATH}"
  rm -rf "${LIBEXEC_DIR}"
  rm -f "${SONIC_HOOK_DIR}/container-hook.sh"
  log "watcher removed; ${STATE_DIR}/libsai_opi_adapter.so left in place"
}

case "${ACTION}" in
  install) install_hook ;;
  rebind)
    require_root
    [[ -x "${LIBEXEC_DIR}/rebind.sh" ]] || die "not installed; run without --rebind-now first"
    exec "${LIBEXEC_DIR}/rebind.sh"
    ;;
  watch)
    require_root
    [[ -x "${LIBEXEC_DIR}/watch.sh" ]] || die "not installed; run without --watch-foreground first"
    exec "${LIBEXEC_DIR}/watch.sh"
    ;;
  uninstall) uninstall_hook ;;
esac
