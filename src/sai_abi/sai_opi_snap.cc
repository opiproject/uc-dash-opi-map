#include "sai_abi/sai_opi_snap.h"

#include "dash_opi/adapter.h"
#include "dash_opi/log.h"
#include "dash_opi/status.h"

#include "security/v1/ipsec.grpc.pb.h"
#include "storage/v1/frontend_nvme.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <string>

namespace storage = opi_api::storage::v1;
namespace ipsec = opi_api::security::v1;

namespace {

const char* OrEnv(const char* arg, const char* env_key, const char* fallback) {
  if (arg != nullptr && arg[0] != '\0') {
    return arg;
  }
  if (const char* e = std::getenv(env_key)) {
    if (e[0] != '\0') {
      return e;
    }
  }
  return fallback;
}

}  // namespace

extern "C" sai_status_t sai_opi_create_nvme_controller(
    const char* parent, const char* nvme_controller_id) {
  auto& adapter = dash_opi::Adapter::Instance();
  storage::FrontendNvmeService::Stub* stub = adapter.nvme();
  if (stub == nullptr) {
    dash_opi::LogXlate("CreateNvmeController skipped: SNAP channel not connected");
    return SAI_STATUS_UNINITIALIZED;
  }

  const char* parent_s =
      OrEnv(parent, "SAI_OPI_NVME_PARENT", "nvmeSubsystems/subsystem2");
  const char* id_s =
      OrEnv(nvme_controller_id, "SAI_OPI_NVME_CONTROLLER_ID", "controller1");

  storage::CreateNvmeControllerRequest req;
  req.set_parent(parent_s);
  req.set_nvme_controller_id(id_s);

  storage::NvmeController* ctrl = req.mutable_nvme_controller();
  storage::NvmeControllerSpec* spec = ctrl->mutable_spec();
  spec->set_nvme_controller_id(2);
  spec->set_trtype(storage::NVME_TRANSPORT_TYPE_PCIE);
  spec->set_max_nsq(5);
  spec->set_max_ncq(5);
  storage::PciEndpoint* pcie = spec->mutable_pcie_id();
  pcie->mutable_port_id()->set_value(0);
  pcie->mutable_physical_function()->set_value(0);
  pcie->mutable_virtual_function()->set_value(0);

  storage::NvmeController resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + adapter.rpc_deadline());
  const grpc::Status grpc_st = stub->CreateNvmeController(&ctx, req, &resp);
  const sai_status_t st = dash_opi::SaiStatusFromGrpc(grpc_st);
  dash_opi::LogXlate(std::string("CreateNvmeController parent=") + parent_s +
                     " nvme_controller_id=" + id_s + " name=" + resp.name() +
                     " grpc_status=" + std::to_string(st) +
                     (grpc_st.ok() ? "" : " err=" + grpc_st.error_message()));
  return st;
}

extern "C" sai_status_t sai_opi_enable_ipsec(const char* ike_name) {
  auto& adapter = dash_opi::Adapter::Instance();
  ipsec::IPsecService::Stub* stub = adapter.ipsec();
  if (stub == nullptr) {
    dash_opi::LogXlate("IPsec skipped: SNAP channel not connected");
    return SAI_STATUS_UNINITIALIZED;
  }

  const char* ike = OrEnv(ike_name, "SAI_OPI_IPSEC_IKE", "ike");

  ipsec::IPsecInitiateRequest init_req;
  init_req.set_ike(ike);
  ipsec::IPsecInitiateResponse init_resp;
  grpc::ClientContext init_ctx;
  init_ctx.set_deadline(std::chrono::system_clock::now() +
                        adapter.rpc_deadline());
  const grpc::Status init_st =
      stub->IPsecInitiate(&init_ctx, init_req, &init_resp);
  const sai_status_t init_sai = dash_opi::SaiStatusFromGrpc(init_st);
  dash_opi::LogXlate(std::string("IPsecInitiate ike=") + ike +
                     " grpc_status=" + std::to_string(init_sai) +
                     (init_st.ok() ? "" : " err=" + init_st.error_message()));

  // Wire type is IPsecRekeyRequest (SNAP). Informal name: CreateIPsecRekeyRequest.
  using CreateIPsecRekeyRequest = ipsec::IPsecRekeyRequest;
  CreateIPsecRekeyRequest rekey_req;
  rekey_req.set_ike(ike);
  ipsec::IPsecRekeyResponse rekey_resp;
  grpc::ClientContext rekey_ctx;
  rekey_ctx.set_deadline(std::chrono::system_clock::now() +
                         adapter.rpc_deadline());
  const grpc::Status rekey_st =
      stub->IPsecRekey(&rekey_ctx, rekey_req, &rekey_resp);
  const sai_status_t rekey_sai = dash_opi::SaiStatusFromGrpc(rekey_st);
  dash_opi::LogXlate(std::string("IPsecRekey ike=") + ike + " success=" +
                     rekey_resp.success() +
                     " grpc_status=" + std::to_string(rekey_sai) +
                     (rekey_st.ok() ? "" : " err=" + rekey_st.error_message()));

  if (init_sai == SAI_STATUS_SUCCESS || rekey_sai == SAI_STATUS_SUCCESS) {
    return SAI_STATUS_SUCCESS;
  }
  if (init_sai == SAI_STATUS_NOT_IMPLEMENTED) {
    return rekey_sai;
  }
  return init_sai;
}
