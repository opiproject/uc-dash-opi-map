#include "dash_opi/inventory_client.h"

#include "dash_opi/log.h"
#include "dash_opi/status.h"

#include <grpcpp/grpcpp.h>

#include <initializer_list>

namespace dash_opi {
namespace {

std::string FirstNonEmpty(std::initializer_list<std::string> xs) {
  for (const auto& s : xs) {
    if (!s.empty() && s != "Unspecified" && s != "Not Specified" &&
        s != "None") {
      return s;
    }
  }
  return {};
}

}  // namespace

OpiInventoryClient::OpiInventoryClient(
    opi_api::inventory::v1::InventoryService::Stub* stub)
    : stub_(stub) {}

DpuIdentity OpiInventoryClient::FromInventory(
    const opi_api::inventory::v1::Inventory& inv) {
  DpuIdentity id;
  const auto& sys = inv.system();
  const auto& board = inv.baseboard();
  const auto& bios = inv.bios();
  std::string pci_product;
  if (inv.pci_size() > 0) {
    pci_product = inv.pci(0).product();
  }
  id.sku = FirstNonEmpty({sys.sku(), sys.family(), board.product(), pci_product});
  id.serial = FirstNonEmpty(
      {sys.serial_number(), board.serial_number(), inv.chassis().serial_number()});
  id.firmware = FirstNonEmpty({bios.version(), sys.version()});
  return id;
}

sai_status_t OpiInventoryClient::GetAndValidate(
    std::chrono::milliseconds deadline, DpuIdentity* out) const {
  if (stub_ == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  opi_api::inventory::v1::GetInventoryRequest req;
  opi_api::inventory::v1::Inventory inv;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + deadline);
  const grpc::Status grpc_st = stub_->GetInventory(&ctx, req, &inv);
  const sai_status_t st = SaiStatusFromGrpc(grpc_st);

  DpuIdentity id;
  if (grpc_st.ok()) {
    id = FromInventory(inv);
  }
  if (out != nullptr) {
    *out = id;
  }

  LogXlate(std::string("GetInventory sku=") +
           (id.sku.empty() ? "unspecified" : id.sku) + " serial=" +
           (id.serial.empty() ? "unspecified" : id.serial) + " firmware=" +
           (id.firmware.empty() ? "unspecified" : id.firmware) +
           " grpc_status=" + std::to_string(st) +
           (grpc_st.ok() ? "" : " err=" + grpc_st.error_message()));

  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  if (id.sku.empty() || id.serial.empty() || id.firmware.empty()) {
    LogXlate("GetInventory validation warning: missing SKU/serial/firmware "
             "on silicon SMBIOS");
  }
  return SAI_STATUS_SUCCESS;
}

}  // namespace dash_opi
