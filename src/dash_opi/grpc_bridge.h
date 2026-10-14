#pragma once

#include "dash_opi/status.h"

#include <grpcpp/grpcpp.h>

#include <chrono>

namespace dash_opi {

// Blocks the SAI calling thread on a generated unary stub method.
// Uses the classic sync API so Ubuntu jammy gRPC 1.30 (no Stub::async())
// and upstream gRPC 1.32+ both compile.
template <typename Stub, typename Request, typename Response>
sai_status_t BlockingUnary(
    Stub* stub,
    grpc::Status (Stub::*rpc)(grpc::ClientContext*, const Request&, Response*),
    const Request& request,
    Response* response,
    std::chrono::milliseconds deadline) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + deadline);
  return StatusFromGrpc((stub->*rpc)(&ctx, request, response));
}

}  // namespace dash_opi
