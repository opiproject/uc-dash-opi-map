#pragma once

extern "C" {
#include <sai.h>
}

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

namespace dash_opi {

// Southbound gRPC targets owned by one host adapter (one libsai.so).
// CloudInfra is DASH networking; SNAP is inventory / NVMe / IPsec.
struct OpiEndpoints {
  std::string cloudinfra;  // typically host:50051 (dpu_cloudinfra_bridge)
  std::string snap;        // typically host:50052 (opi-nvidia-bridge)
};

/** env → /etc/sonic/opi.env → /etc/sai.d/opi.env → sai.profile. */
std::string ConfigValue(const char* key,
                        const sai_service_method_table_t* services = nullptr);

/** Replace the port on host:port (IPv4 or [IPv6]:port). */
std::string SiblingHostPort(const std::string& hostport, const char* port);

OpiEndpoints ResolveOpiEndpoints(
    const sai_service_method_table_t* services = nullptr);

/** SNAP target: OPI_SNAP_ADDRESS, else sibling :50052 of cloudinfra. */
std::string ResolveSnapTarget(
    const std::string& cloudinfra,
    const sai_service_method_table_t* services = nullptr);

std::shared_ptr<grpc::Channel> OpenInsecureChannel(
    const std::string& target, std::chrono::milliseconds wait);

bool EnvFlagSet(const char* name);

}  // namespace dash_opi
