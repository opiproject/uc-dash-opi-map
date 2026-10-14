#pragma once

#include "dash_opi/object_store.h"

#include "inventory/inventory.grpc.pb.h"
#include "network/cloud/cloudrpc.grpc.pb.h"
#include "security/v1/ipsec.grpc.pb.h"
#include "storage/v1/frontend_nvme.grpc.pb.h"
#include "telemetry/v1/telemetry.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>

extern "C" {
#include <sai.h>
}

namespace dash_opi {

class Adapter {
 public:
  static Adapter& Instance();

  ObjectStore& store() { return store_; }
  const ObjectStore& store() const { return store_; }

  using CloudStub =
      opi_api::network::cloud::v1alpha1::CloudInfraService::Stub;
  using TelemetryStub = opi_api::telemetry::v1::TelemetryService::Stub;
  using InventoryStub = opi_api::inventory::v1::InventoryService::Stub;
  using NvmeStub = opi_api::storage::v1::FrontendNvmeService::Stub;
  using IPsecStub = opi_api::security::v1::IPsecService::Stub;

  CloudStub* stub() { return stub_.get(); }
  TelemetryStub* telemetry() { return telemetry_stub_.get(); }
  InventoryStub* inventory() { return inventory_stub_.get(); }
  NvmeStub* nvme() { return nvme_stub_.get(); }
  IPsecStub* ipsec() { return ipsec_stub_.get(); }

  /** cloudinfra required; snap optional unless OPI_REQUIRE_SNAP=1. */
  sai_status_t Connect(const std::string& cloudinfra,
                       const std::string& snap = {});
  sai_status_t ConnectFromConfig(
      const sai_service_method_table_t* services = nullptr);
  void Shutdown();

  std::chrono::milliseconds rpc_deadline() const { return rpc_deadline_; }
  const std::string& opi_parent() const { return opi_parent_; }
  const std::string& cloudinfra_target() const { return cloudinfra_target_; }
  const std::string& snap_target() const { return snap_target_; }

 private:
  sai_status_t ConnectSnap(const std::string& snap);

  ObjectStore store_;
  std::shared_ptr<grpc::Channel> channel_;
  std::shared_ptr<grpc::Channel> snap_channel_;
  std::unique_ptr<CloudStub> stub_;
  std::unique_ptr<TelemetryStub> telemetry_stub_;
  std::unique_ptr<InventoryStub> inventory_stub_;
  std::unique_ptr<NvmeStub> nvme_stub_;
  std::unique_ptr<IPsecStub> ipsec_stub_;
  std::chrono::milliseconds rpc_deadline_{5000};
  std::string opi_parent_;
  std::string cloudinfra_target_;
  std::string snap_target_;
};

}  // namespace dash_opi
