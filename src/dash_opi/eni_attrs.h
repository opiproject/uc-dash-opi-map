#pragma once

extern "C" {
#include <sai.h>
#include <saiexperimentaldasheni.h>
}

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace dash_opi {

// Published OCP SAI (saiexperimentaldasheni.h) has no SAI_ENI_ATTR_VM_MAC.
// Overlay MAC is sai_eni_ether_address_map_entry_t.address; SONiC
// DASH_ENI_TABLE.mac_address is that map key. Accept a vendor-extension
// attribute so create_eni can pack VnicSpec.mac_address / HostIfSpec.mac_address
// when the MAC is known at ENI create time.
#ifndef SAI_ENI_ATTR_VM_MAC
#define SAI_ENI_ATTR_VM_MAC SAI_ENI_ATTR_CUSTOM_RANGE_START
#endif

struct EniCreateAttrs {
  sai_object_id_t vnet_id{SAI_NULL_OBJECT_ID};
  uint32_t cps{0};
  bool has_cps{false};
  sai_mac_t vm_mac{};
  bool has_vm_mac{false};
  sai_dash_eni_mode_t mode{SAI_DASH_ENI_MODE_VM};
  bool admin_state{false};
  uint32_t vm_vni{0};
  bool has_vm_vni{false};

  std::optional<uint32_t> error_attr_index;
};

inline EniCreateAttrs ParseEniCreateAttrs(uint32_t attr_count,
                                          const sai_attribute_t* attr_list) {
  EniCreateAttrs out;
  if (attr_count > 0 && attr_list == nullptr) {
    out.error_attr_index = 0;
    return out;
  }

  for (uint32_t i = 0; i < attr_count; ++i) {
    switch (attr_list[i].id) {
      case SAI_ENI_ATTR_VNET_ID:
        out.vnet_id = attr_list[i].value.oid;
        break;
      case SAI_ENI_ATTR_CPS:
        out.cps = attr_list[i].value.u32;
        out.has_cps = true;
        break;
      case SAI_ENI_ATTR_VM_MAC:
        std::memcpy(out.vm_mac, attr_list[i].value.mac, sizeof(sai_mac_t));
        out.has_vm_mac = true;
        break;
      case SAI_ENI_ATTR_DASH_ENI_MODE:
        out.mode = static_cast<sai_dash_eni_mode_t>(attr_list[i].value.s32);
        break;
      case SAI_ENI_ATTR_ADMIN_STATE:
        out.admin_state = attr_list[i].value.booldata;
        break;
      case SAI_ENI_ATTR_VM_VNI:
        out.vm_vni = attr_list[i].value.u32;
        out.has_vm_vni = true;
        break;
      default:
        break;
    }
  }
  return out;
}

inline std::string MacToBytes(const sai_mac_t mac) {
  return {reinterpret_cast<const char*>(mac), sizeof(sai_mac_t)};
}

}  // namespace dash_opi
