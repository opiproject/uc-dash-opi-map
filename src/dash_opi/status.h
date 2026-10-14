#pragma once

extern "C" {
#include <sai.h>
}

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <optional>

namespace dash_opi {

/**
 * Map a gRPC status code to a SAI C enum orchagent already switches on.
 *
 * Exact pairs required by the DASH overlay path:
 *   grpc::StatusCode::OK                  → SAI_STATUS_SUCCESS
 *   grpc::StatusCode::NOT_FOUND           → SAI_STATUS_ITEM_NOT_FOUND
 *   grpc::StatusCode::RESOURCE_EXHAUSTED  → SAI_STATUS_TABLE_FULL
 *
 * Every other gRPC code becomes a defined sai_status_t (never an undefined
 * integer). orchagent treats unknown codes as fatal; do not invent values.
 */
sai_status_t SaiStatusFromGrpcCode(grpc::StatusCode code);

/** Same mapping, from a full grpc::Status. Optional attr index for INVALID_ARGUMENT. */
sai_status_t SaiStatusFromGrpc(const grpc::Status& status,
                               std::optional<uint32_t> attr_index = std::nullopt);

// Back-compat name used by grpc_bridge.h.
inline sai_status_t StatusFromGrpc(
    const grpc::Status& status,
    std::optional<uint32_t> attr_index = std::nullopt) {
  return SaiStatusFromGrpc(status, attr_index);
}

}  // namespace dash_opi
