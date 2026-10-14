#include "dash_opi/status.h"

namespace dash_opi {

sai_status_t SaiStatusFromGrpcCode(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK:
      return SAI_STATUS_SUCCESS;
    case grpc::StatusCode::NOT_FOUND:
      return SAI_STATUS_ITEM_NOT_FOUND;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      // DPU table / resource cap — orchagent handles TABLE_FULL on create.
      return SAI_STATUS_TABLE_FULL;
    case grpc::StatusCode::ALREADY_EXISTS:
      return SAI_STATUS_ITEM_ALREADY_EXISTS;
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::OUT_OF_RANGE:
      return SAI_STATUS_INVALID_PARAMETER;
    case grpc::StatusCode::FAILED_PRECONDITION:
      return SAI_STATUS_OBJECT_IN_USE;
    case grpc::StatusCode::UNIMPLEMENTED:
      return SAI_STATUS_NOT_IMPLEMENTED;
    default:
      // CANCELLED, UNKNOWN, DEADLINE_EXCEEDED, PERMISSION_DENIED, ABORTED,
      // INTERNAL, UNAVAILABLE, DATA_LOSS, UNAUTHENTICATED, …
      return SAI_STATUS_FAILURE;
  }
}

sai_status_t SaiStatusFromGrpc(const grpc::Status& status,
                               std::optional<uint32_t> attr_index) {
  if (status.error_code() == grpc::StatusCode::INVALID_ARGUMENT && attr_index) {
    return static_cast<sai_status_t>(SAI_STATUS_INVALID_ATTR_VALUE_0 +
                                     *attr_index);
  }
  return SaiStatusFromGrpcCode(status.error_code());
}

}  // namespace dash_opi
