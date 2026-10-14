#pragma once

#include "inventory/inventory.grpc.pb.h"

#include <chrono>
#include <string>

extern "C" {
#include <sai.h>
}

namespace dash_opi {

struct DpuIdentity {
  std::string sku;
  std::string serial;
  std::string firmware;
};

/**
 * InventoryService client (SNAP :50052). GetInventory is SMBIOS/FRU from
 * silicon — SKU, serial, BIOS/firmware — not dataplane counters.
 */
class OpiInventoryClient {
 public:
  explicit OpiInventoryClient(
      opi_api::inventory::v1::InventoryService::Stub* stub);

  static DpuIdentity FromInventory(const opi_api::inventory::v1::Inventory& inv);

  sai_status_t GetAndValidate(std::chrono::milliseconds deadline,
                              DpuIdentity* out) const;

 private:
  opi_api::inventory::v1::InventoryService::Stub* stub_;
};

}  // namespace dash_opi
