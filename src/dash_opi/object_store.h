#pragma once

extern "C" {
#include <sai.h>
}

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dash_opi {

// Maps SAI object ids to OPI resource names (vnic/{id}, vpcs/{id}, ...).
// Never hold mu_ across a gRPC wait.
class ObjectStore {
 public:
  sai_object_id_t Alloc(sai_object_type_t type, sai_object_id_t switch_id);

  void Bind(sai_object_id_t oid, std::string resource_name);
  void Unbind(sai_object_id_t oid);

  // Host PF/VF Interface name created with this ENI (`interfaces/{id}`).
  void BindHostIf(sai_object_id_t eni_oid, std::string host_if_name);
  std::optional<std::string> HostIfName(sai_object_id_t eni_oid) const;

  std::optional<std::string> Name(sai_object_id_t oid) const;
  std::optional<sai_object_id_t> Oid(std::string_view name) const;

  // VNET create records vpc + default subnet; ENI create looks the subnet up.
  void BindVnet(sai_object_id_t vnet_oid,
                std::string vpc_name,
                std::string subnet_name);
  std::optional<std::string> SubnetNameForVnet(sai_object_id_t vnet_oid) const;

  // Outbound routing entries are key-based (group oid + prefix), not oid-out.
  void BindRouteEntry(std::string entry_key, std::string route_name);
  void UnbindRouteEntry(std::string_view entry_key);
  std::optional<std::string> RouteNameForEntry(std::string_view entry_key) const;

 private:
  mutable std::shared_mutex mu_;
  std::unordered_map<sai_object_id_t, std::string> oid_to_name_;
  std::unordered_map<std::string, sai_object_id_t> name_to_oid_;
  std::unordered_map<sai_object_id_t, std::string> vnet_to_subnet_;
  std::unordered_map<sai_object_id_t, std::string> eni_to_host_if_;
  std::unordered_map<std::string, std::string> route_entry_to_name_;
  uint64_t next_local_{1};
};

}  // namespace dash_opi
