#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Start the OPI vendor gRPC bridge on a DPU ARM SoC (NVIDIA BlueField / similar).
# opi-nvidia-bridge panics unless Redis is reachable (-redis_addr, default 127.0.0.1:6379).
# This helper starts redis on a docker network, then the bridge with that address.
#
#   ./scripts/start_opi_dpu_bridge.sh
set -euo pipefail

IMAGE="${OPI_BRIDGE_IMAGE:-ghcr.io/opiproject/opi-nvidia-bridge:main}"
CONTAINER_NAME="${OPI_BRIDGE_NAME:-opi-dpu-bridge}"
SOCKET_DIR="${OPI_SOCKET_DIR:-/var/tmp}"
GRPC_PORT="${OPI_GRPC_PORT:-50051}"
GRPC_CLI_IMAGE="${GRPC_CLI_IMAGE:-namely/grpc-cli}"
WAIT_SECONDS="${OPI_BRIDGE_WAIT:-45}"
NETWORK="${OPI_DOCKER_NETWORK:-opi-dpu}"
REDIS_NAME="${OPI_REDIS_NAME:-opi-redis}"
REDIS_IMAGE="${OPI_REDIS_IMAGE:-redis:7.2-alpine}"
HTTP_PORT="${OPI_HTTP_PORT:-8082}"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "error: $*"; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker is required on the DPU"

mkdir -p "${SOCKET_DIR}"
[[ -d "${SOCKET_DIR}" && -r "${SOCKET_DIR}" ]] || die "cannot use socket dir ${SOCKET_DIR}"

ensure_network() {
  docker network inspect "${NETWORK}" >/dev/null 2>&1 || docker network create "${NETWORK}" >/dev/null
}

ensure_redis() {
  if docker inspect -f '{{.State.Running}}' "${REDIS_NAME}" 2>/dev/null | grep -qx true; then
    log "redis ${REDIS_NAME} already running"
    return 0
  fi
  log "pulling ${REDIS_IMAGE}"
  docker pull "${REDIS_IMAGE}"
  docker rm -f "${REDIS_NAME}" >/dev/null 2>&1 || true
  log "starting ${REDIS_NAME} on network ${NETWORK}"
  docker run -d \
    --name "${REDIS_NAME}" \
    --restart unless-stopped \
    --network "${NETWORK}" \
    "${REDIS_IMAGE}"
  local i
  for i in $(seq 1 20); do
    if docker exec "${REDIS_NAME}" redis-cli ping 2>/dev/null | grep -q PONG; then
      log "redis ready"
      return 0
    fi
    sleep 1
  done
  die "redis ${REDIS_NAME} did not answer PING"
}

ensure_network
ensure_redis

log "pulling ${IMAGE}"
docker pull "${IMAGE}"

if docker ps -a --format '{{.Names}}' | grep -qx "${CONTAINER_NAME}"; then
  log "removing existing container ${CONTAINER_NAME}"
  docker rm -f "${CONTAINER_NAME}" >/dev/null
fi

# Official compose: -redis_addr=redis:6379 (not 127.0.0.1 inside the bridge netns).
log "starting ${CONTAINER_NAME}  (${SOCKET_DIR} -> /var/tmp, port ${GRPC_PORT}, redis=${REDIS_NAME}:6379)"
docker run -d \
  --name "${CONTAINER_NAME}" \
  --restart unless-stopped \
  --network "${NETWORK}" \
  -v "${SOCKET_DIR}:/var/tmp/" \
  -p "0.0.0.0:${GRPC_PORT}:50051" \
  -p "${HTTP_PORT}:8082" \
  "${IMAGE}" \
  /opi-nvidia-bridge \
    -grpc_port=50051 \
    -http_port=8082 \
    -spdk_addr /var/tmp/spdk.sock \
    -redis_addr="${REDIS_NAME}:6379"

sleep 3
if ! docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null | grep -qx true; then
  die "container ${CONTAINER_NAME} is not running; logs:\n$(docker logs "${CONTAINER_NAME}" 2>&1 | tail -n 50)"
fi
status="$(docker inspect -f '{{.State.Status}} {{.State.Restarting}}' "${CONTAINER_NAME}" 2>/dev/null || true)"
if printf '%s' "${status}" | grep -qi 'restarting\|true'; then
  die "container ${CONTAINER_NAME} is crash-looping (${status}); logs:\n$(docker logs "${CONTAINER_NAME}" 2>&1 | tail -n 50)"
fi

grpc_cli_ls() {
  local target="127.0.0.1:${GRPC_PORT}"
  if command -v grpc_cli >/dev/null 2>&1; then
    grpc_cli ls --json_input --json_output "${target}" -l
  else
    docker run --rm --network=host "${GRPC_CLI_IMAGE}" \
      ls --json_input --json_output "${target}" -l
  fi
}

port_open() {
  if command -v ss >/dev/null 2>&1 && ss -lnt 2>/dev/null | grep -q ":${GRPC_PORT}"; then
    return 0
  fi
  if command -v nc >/dev/null 2>&1 && nc -z -w 1 127.0.0.1 "${GRPC_PORT}" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

log "waiting up to ${WAIT_SECONDS}s for SNAP gRPC on :${GRPC_PORT}"
deadline=$((SECONDS + WAIT_SECONDS))
while (( SECONDS < deadline )); do
  if docker inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null | grep -qx true; then
    if out="$(grpc_cli_ls 2>/dev/null)"; then
      log "SNAP gRPC is listening on 0.0.0.0:${GRPC_PORT}"
      printf '%s\n' "${out}"
      exit 0
    fi
    if port_open; then
      log "SNAP container up and TCP :${GRPC_PORT} open (grpc-cli skipped; namely/grpc-cli is amd64)"
      exit 0
    fi
  else
    die "container ${CONTAINER_NAME} is not running; logs:\n$(docker logs "${CONTAINER_NAME}" 2>&1 | tail -n 50)"
  fi
  sleep 1
done

log "timed out waiting for SNAP on :${GRPC_PORT}; container logs:"
docker logs "${CONTAINER_NAME}" 2>&1 | tail -n 80
die "SNAP did not listen on 127.0.0.1:${GRPC_PORT}"
