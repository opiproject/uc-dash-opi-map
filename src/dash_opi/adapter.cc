#include "dash_opi/adapter.h"

#include "dash_opi/channel.h"
#include "dash_opi/inventory_client.h"
#include "dash_opi/log.h"

namespace dash_opi {

Adapter& Adapter::Instance() {
  static Adapter adapter;
  return adapter;
}

sai_status_t Adapter::ConnectSnap(const std::string& snap) {
  inventory_stub_.reset();
  nvme_stub_.reset();
  ipsec_stub_.reset();
  snap_channel_.reset();
  snap_target_ = snap;

  if (snap.empty()) {
    LogXlate("SNAP target empty; inventory/NVMe/IPsec disabled");
    return SAI_STATUS_SUCCESS;
  }

  constexpr auto kSnapWait = std::chrono::seconds(2);
  snap_channel_ = OpenInsecureChannel(snap, kSnapWait);
  if (!snap_channel_) {
    LogXlate("SNAP channel unavailable target=" + snap);
    if (EnvFlagSet("OPI_REQUIRE_SNAP") || EnvFlagSet("OPI_REQUIRE_INVENTORY")) {
      return SAI_STATUS_FAILURE;
    }
    return SAI_STATUS_SUCCESS;
  }

  inventory_stub_ =
      opi_api::inventory::v1::InventoryService::NewStub(snap_channel_);
  nvme_stub_ = opi_api::storage::v1::FrontendNvmeService::NewStub(snap_channel_);
  ipsec_stub_ = opi_api::security::v1::IPsecService::NewStub(snap_channel_);
  LogXlate("SNAP connected target=" + snap);

  if (EnvFlagSet("OPI_SKIP_INVENTORY")) {
    return SAI_STATUS_SUCCESS;
  }

  OpiInventoryClient client(inventory_stub_.get());
  const sai_status_t inv = client.GetAndValidate(rpc_deadline_, nullptr);
  if (inv != SAI_STATUS_SUCCESS &&
      (EnvFlagSet("OPI_REQUIRE_SNAP") || EnvFlagSet("OPI_REQUIRE_INVENTORY"))) {
    return inv;
  }
  return SAI_STATUS_SUCCESS;
}

sai_status_t Adapter::Connect(const std::string& cloudinfra,
                              const std::string& snap) {
  Shutdown();
  cloudinfra_target_ = cloudinfra;

  channel_ = OpenInsecureChannel(cloudinfra, std::chrono::seconds(5));
  if (!channel_) {
    LogXlate("CloudInfra channel unavailable target=" + cloudinfra);
    return SAI_STATUS_FAILURE;
  }
  stub_ = opi_api::network::cloud::v1alpha1::CloudInfraService::NewStub(
      channel_);
  telemetry_stub_ = opi_api::telemetry::v1::TelemetryService::NewStub(channel_);
  LogXlate("CloudInfra connected target=" + cloudinfra);

  const std::string snap_target =
      snap.empty() ? ResolveSnapTarget(cloudinfra) : snap;
  return ConnectSnap(snap_target);
}

sai_status_t Adapter::ConnectFromConfig(
    const sai_service_method_table_t* services) {
  const OpiEndpoints eps = ResolveOpiEndpoints(services);
  return Connect(eps.cloudinfra, eps.snap);
}

void Adapter::Shutdown() {
  stub_.reset();
  telemetry_stub_.reset();
  inventory_stub_.reset();
  nvme_stub_.reset();
  ipsec_stub_.reset();
  channel_.reset();
  snap_channel_.reset();
  cloudinfra_target_.clear();
  snap_target_.clear();
}

}  // namespace dash_opi
