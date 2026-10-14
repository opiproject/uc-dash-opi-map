#include "dash_opi/object_store.h"

#include <mutex>
#include <utility>

namespace dash_opi {

sai_object_id_t ObjectStore::Alloc(sai_object_type_t type,
                                   sai_object_id_t switch_id) {
  std::unique_lock lock(mu_);
  const uint64_t local = next_local_++;
  // SAI-style: object type in the high bits, local allocator in the low bits.
  (void)switch_id;
  return (static_cast<uint64_t>(type) << 48) | local;
}

void ObjectStore::Bind(sai_object_id_t oid, std::string resource_name) {
  std::unique_lock lock(mu_);
  name_to_oid_[resource_name] = oid;
  oid_to_name_[oid] = std::move(resource_name);
}

void ObjectStore::Unbind(sai_object_id_t oid) {
  std::unique_lock lock(mu_);
  if (auto it = oid_to_name_.find(oid); it != oid_to_name_.end()) {
    name_to_oid_.erase(it->second);
    oid_to_name_.erase(it);
  }
  vnet_to_subnet_.erase(oid);
  eni_to_host_if_.erase(oid);
}

void ObjectStore::BindHostIf(sai_object_id_t eni_oid, std::string host_if_name) {
  std::unique_lock lock(mu_);
  if (host_if_name.empty()) {
    eni_to_host_if_.erase(eni_oid);
    return;
  }
  eni_to_host_if_[eni_oid] = std::move(host_if_name);
}

std::optional<std::string> ObjectStore::HostIfName(sai_object_id_t eni_oid) const {
  std::shared_lock lock(mu_);
  if (auto it = eni_to_host_if_.find(eni_oid); it != eni_to_host_if_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<std::string> ObjectStore::Name(sai_object_id_t oid) const {
  std::shared_lock lock(mu_);
  if (auto it = oid_to_name_.find(oid); it != oid_to_name_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<sai_object_id_t> ObjectStore::Oid(std::string_view name) const {
  std::shared_lock lock(mu_);
  if (auto it = name_to_oid_.find(std::string(name)); it != name_to_oid_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void ObjectStore::BindVnet(sai_object_id_t vnet_oid,
                           std::string vpc_name,
                           std::string subnet_name) {
  std::unique_lock lock(mu_);
  name_to_oid_[vpc_name] = vnet_oid;
  oid_to_name_[vnet_oid] = std::move(vpc_name);
  vnet_to_subnet_[vnet_oid] = std::move(subnet_name);
}

std::optional<std::string> ObjectStore::SubnetNameForVnet(
    sai_object_id_t vnet_oid) const {
  std::shared_lock lock(mu_);
  if (auto it = vnet_to_subnet_.find(vnet_oid); it != vnet_to_subnet_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void ObjectStore::BindRouteEntry(std::string entry_key, std::string route_name) {
  std::unique_lock lock(mu_);
  route_entry_to_name_[std::move(entry_key)] = std::move(route_name);
}

void ObjectStore::UnbindRouteEntry(std::string_view entry_key) {
  std::unique_lock lock(mu_);
  route_entry_to_name_.erase(std::string(entry_key));
}

std::optional<std::string> ObjectStore::RouteNameForEntry(
    std::string_view entry_key) const {
  std::shared_lock lock(mu_);
  if (auto it = route_entry_to_name_.find(std::string(entry_key));
      it != route_entry_to_name_.end()) {
    return it->second;
  }
  return std::nullopt;
}

}  // namespace dash_opi
