#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Generate Go stubs from proto/ into api/.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
PROTO_ROOT="${PROTO_ROOT:-${ROOT}/proto}"
API="${HERE}/api"
MODULE="dash-opi-map/dpu-cloudinfra/api"

command -v protoc >/dev/null || {
  echo "protoc is required (apt-get install protobuf-compiler)" >&2
  exit 1
}

mkdir -p "${API}"
protoc -I "${PROTO_ROOT}" -I /usr/include \
  --go_out="${API}" --go_opt=module="${MODULE}" \
  --go-grpc_out="${API}" --go-grpc_opt=module="${MODULE}" \
  network/cloud/cloudrpc.proto \
  network/cloud/interface.proto \
  network/cloud/vnic.proto \
  network/cloud/vpc.proto \
  network/cloud/subnet.proto \
  network/cloud/route.proto \
  network/cloud/networkpolicy.proto \
  network/opinetcommon/networktypes.proto \
  telemetry/v1/telemetry.proto

echo "generated stubs under ${API}"
