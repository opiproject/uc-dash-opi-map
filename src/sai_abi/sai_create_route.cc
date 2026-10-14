#include "sai_abi/sai_create_route.h"

#include "dash_opi/adapter.h"
#include "dash_opi/grpc_bridge.h"
#include "dash_opi/log.h"

#include <network/cloud/cloudrpc.grpc.pb.h>
#include <network/cloud/route.pb.h>
#include <network/opinetcommon/networktypes.pb.h>

#include <google/protobuf/empty.pb.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

extern "C" {
#include <saiextensions.h>
#include <saiexperimentaldashoutboundrouting.h>
}

namespace cloud = opi_api::network::cloud::v1alpha1;
namespace opinet = opi_api::network::opinetcommon::v1alpha1;

namespace {

std::string HexId(sai_object_id_t oid) {
  std::ostringstream os;
  os << std::hex << (oid & 0x0000ffffffffffffULL);
  return os.str();
}

std::string PrefixToString(const sai_ip_prefix_t& pfx) {
  char buf[64];
  if (pfx.addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
    const uint32_t host = ntohl(pfx.addr.ip4);
    const unsigned a = (host >> 24) & 0xff;
    const unsigned b = (host >> 16) & 0xff;
    const unsigned c = (host >> 8) & 0xff;
    const unsigned d = host & 0xff;
    unsigned plen = 0;
    uint32_t mask = ntohl(pfx.mask.ip4);
    while (mask & 0x80000000u) {
      ++plen;
      mask <<= 1;
    }
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u/%u", a, b, c, d, plen);
    return buf;
  }
  return "0.0.0.0/0";
}

bool PackIpv4Prefix(const sai_ip_prefix_t& pfx, opinet::IPPrefix* out) {
  if (pfx.addr_family != SAI_IP_ADDR_FAMILY_IPV4 || out == nullptr) {
    return false;
  }
  out->mutable_addr()->set_af(opinet::IP_AF_INET);
  // OPI fixed32 v4_addr is host-order in many bridges; store network-order
  // bits as uint32 matching sai_ip4_t wire value.
  out->mutable_addr()->set_v4_addr(ntohl(pfx.addr.ip4));
  unsigned plen = 0;
  uint32_t mask = ntohl(pfx.mask.ip4);
  while (mask & 0x80000000u) {
    ++plen;
    mask <<= 1;
  }
  out->set_len(static_cast<int32_t>(plen));
  return true;
}

std::string RouteEntryKey(sai_object_id_t group_oid,
                          const sai_ip_prefix_t& destination) {
  return HexId(group_oid) + "|" + PrefixToString(destination);
}

}  // namespace

extern "C" sai_status_t sai_create_outbound_routing_group(
    sai_object_id_t* outbound_routing_group_id,
    sai_object_id_t switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  if (outbound_routing_group_id == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  *outbound_routing_group_id = SAI_NULL_OBJECT_ID;

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  bool disabled = false;
  for (uint32_t i = 0; i < attr_count; ++i) {
    if (attr_list[i].id == SAI_OUTBOUND_ROUTING_GROUP_ATTR_DISABLED) {
      disabled = attr_list[i].value.booldata;
    }
  }
  (void)disabled;  // OPI RouteTable has no disabled flag; still create.

  const sai_object_id_t oid = adapter.store().Alloc(
      static_cast<sai_object_type_t>(SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP),
      switch_id);
  const std::string id = HexId(oid);
  const std::string table_name = "routetables/" + id;

  cloud::CreateRouteTableRequest req;
  req.set_parent(adapter.opi_parent());
  req.set_routetable_id(id);
  cloud::RouteTable* table = req.mutable_routetable();
  table->set_name(table_name);
  table->mutable_spec()->set_af(opinet::IP_AF_INET);

  cloud::RouteTable resp;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateRouteTable, req, &resp,
      adapter.rpc_deadline());
  const std::string out_name =
      resp.name().empty() ? table_name : resp.name();
  dash_opi::LogXlate("CreateRouteTable name=" + out_name +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().Bind(oid, out_name);
  *outbound_routing_group_id = oid;
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_remove_outbound_routing_group(
    sai_object_id_t outbound_routing_group_id) {
  if (outbound_routing_group_id == SAI_NULL_OBJECT_ID) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }
  const auto name = adapter.store().Name(outbound_routing_group_id);
  if (!name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }
  cloud::DeleteRouteTableRequest del;
  del.set_name(*name);
  del.set_allow_missing(true);
  google::protobuf::Empty unused;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteRouteTable, del, &unused,
      adapter.rpc_deadline());
  dash_opi::LogXlate("DeleteRouteTable name=" + *name +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().Unbind(outbound_routing_group_id);
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_create_outbound_routing_entry(
    const sai_outbound_routing_entry_t* outbound_routing_entry,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  if (outbound_routing_entry == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  const auto table_name =
      adapter.store().Name(outbound_routing_entry->outbound_routing_group_id);
  if (!table_name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }

  sai_outbound_routing_entry_action_t action =
      SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET;
  sai_object_id_t dst_vnet = SAI_NULL_OBJECT_ID;
  for (uint32_t i = 0; i < attr_count; ++i) {
    switch (attr_list[i].id) {
      case SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION:
        action = static_cast<sai_outbound_routing_entry_action_t>(
            attr_list[i].value.s32);
        break;
      case SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID:
        dst_vnet = attr_list[i].value.oid;
        break;
      default:
        break;
    }
  }

  const std::string entry_key = RouteEntryKey(
      outbound_routing_entry->outbound_routing_group_id,
      outbound_routing_entry->destination);
  const std::string route_id = entry_key;
  // Replace '|' for a legal resource id segment.
  std::string safe_id = route_id;
  for (char& c : safe_id) {
    if (c == '|' || c == '/') {
      c = '_';
    }
  }
  const std::string route_name = "routes/" + safe_id;

  cloud::CreateRouteRequest req;
  req.set_parent(adapter.opi_parent());
  req.set_route_id(safe_id);
  cloud::Route* route = req.mutable_route();
  route->set_name(route_name);
  cloud::RouteSpec* spec = route->mutable_spec();
  spec->set_route_table_name_ref(*table_name);
  cloud::RouteAttrs* attrs = spec->mutable_attrs();
  if (!PackIpv4Prefix(outbound_routing_entry->destination,
                      attrs->mutable_prefix())) {
    return SAI_STATUS_INVALID_PARAMETER;
  }

  switch (action) {
    case SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET:
    case SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT:
      attrs->set_route_class(cloud::ROUTE_CLASS_VPC);
      if (dst_vnet != SAI_NULL_OBJECT_ID) {
        if (auto vpc = adapter.store().Name(dst_vnet)) {
          attrs->mutable_vpc_nh_info()->set_vpc_name_ref(*vpc);
        }
      }
      if (action == SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET &&
          !attrs->has_vpc_nh_info()) {
        attrs->set_eval_mapping(true);
      }
      break;
    case SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_SERVICE_TUNNEL:
      attrs->set_route_class(cloud::ROUTE_CLASS_SERVICE);
      break;
    case SAI_OUTBOUND_ROUTING_ENTRY_ACTION_DROP:
      // No first-class drop on Route; still record prefix with unspecified class.
      attrs->set_route_class(cloud::ROUTE_CLASS_UNSPECIFIED);
      break;
    default:
      attrs->set_route_class(cloud::ROUTE_CLASS_VPC);
      attrs->set_eval_mapping(true);
      break;
  }

  cloud::Route resp;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateRoute, req, &resp,
      adapter.rpc_deadline());
  const std::string out_name =
      resp.name().empty() ? route_name : resp.name();
  dash_opi::LogXlate("CreateRoute name=" + out_name +
                     " table=" + *table_name +
                     " prefix=" + PrefixToString(outbound_routing_entry->destination) +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().BindRouteEntry(entry_key, out_name);
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_remove_outbound_routing_entry(
    const sai_outbound_routing_entry_t* outbound_routing_entry) {
  if (outbound_routing_entry == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }
  const std::string entry_key = RouteEntryKey(
      outbound_routing_entry->outbound_routing_group_id,
      outbound_routing_entry->destination);
  const auto route_name = adapter.store().RouteNameForEntry(entry_key);
  if (!route_name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }
  cloud::DeleteRouteRequest del;
  del.set_name(*route_name);
  del.set_allow_missing(true);
  google::protobuf::Empty unused;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteRoute, del, &unused,
      adapter.rpc_deadline());
  dash_opi::LogXlate("DeleteRoute name=" + *route_name +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().UnbindRouteEntry(entry_key);
  return SAI_STATUS_SUCCESS;
}
