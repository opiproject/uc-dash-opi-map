#include "sai_abi/sai_create_acl.h"

#include "dash_opi/adapter.h"
#include "dash_opi/grpc_bridge.h"
#include "dash_opi/log.h"

#include <network/cloud/cloudrpc.grpc.pb.h>
#include <network/cloud/networkpolicy.pb.h>
#include <network/opinetcommon/networktypes.pb.h>

#include <google/protobuf/empty.pb.h>

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
#include <saiexperimentaldashacl.h>
}

namespace cloud = opi_api::network::cloud::v1alpha1;
namespace opinet = opi_api::network::opinetcommon::v1alpha1;

namespace {

std::string HexId(sai_object_id_t oid) {
  std::ostringstream os;
  os << std::hex << (oid & 0x0000ffffffffffffULL);
  return os.str();
}

bool PackIpv4Prefix(const sai_ip_prefix_t& pfx, opinet::IPPrefix* out) {
  if (pfx.addr_family != SAI_IP_ADDR_FAMILY_IPV4 || out == nullptr) {
    return false;
  }
  out->mutable_addr()->set_af(opinet::IP_AF_INET);
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

bool FirstPrefix(const sai_ip_prefix_list_t& list, sai_ip_prefix_t* out) {
  if (list.count == 0 || list.list == nullptr || out == nullptr) {
    return false;
  }
  *out = list.list[0];
  return true;
}

}  // namespace

extern "C" sai_status_t sai_create_dash_acl_group(
    sai_object_id_t* dash_acl_group_id,
    sai_object_id_t switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  if (dash_acl_group_id == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  *dash_acl_group_id = SAI_NULL_OBJECT_ID;

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  sai_ip_addr_family_t af = SAI_IP_ADDR_FAMILY_IPV4;
  for (uint32_t i = 0; i < attr_count; ++i) {
    if (attr_list[i].id == SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY) {
      af = static_cast<sai_ip_addr_family_t>(attr_list[i].value.s32);
    }
  }

  const sai_object_id_t oid = adapter.store().Alloc(
      static_cast<sai_object_type_t>(SAI_OBJECT_TYPE_DASH_ACL_GROUP), switch_id);
  const std::string id = HexId(oid);
  const std::string policy_name = "securitypolicys/" + id;

  cloud::CreateSecurityPolicyRequest req;
  req.set_parent(adapter.opi_parent());
  req.set_securitypolicy_id(id);
  cloud::SecurityPolicy* policy = req.mutable_securitypolicy();
  policy->set_name(policy_name);
  cloud::SecurityPolicySpec* spec = policy->mutable_spec();
  spec->set_type(cloud::SECURITY_POLICY_TYPE_TENANT);
  spec->set_stateless(false);
  spec->set_rule_format(cloud::POLICY_RULE_FORMAT_FLATTENED);
  spec->set_address_family(af == SAI_IP_ADDR_FAMILY_IPV6 ? opinet::IP_AF_INET6
                                                         : opinet::IP_AF_INET);

  cloud::SecurityPolicy resp;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateSecurityPolicy, req, &resp,
      adapter.rpc_deadline());
  const std::string out_name =
      resp.name().empty() ? policy_name : resp.name();
  dash_opi::LogXlate("CreateSecurityPolicy name=" + out_name +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().Bind(oid, out_name);
  *dash_acl_group_id = oid;
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_remove_dash_acl_group(
    sai_object_id_t dash_acl_group_id) {
  if (dash_acl_group_id == SAI_NULL_OBJECT_ID) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }
  const auto name = adapter.store().Name(dash_acl_group_id);
  if (!name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }
  cloud::DeleteSecurityPolicyRequest del;
  del.set_name(*name);
  del.set_allow_missing(true);
  google::protobuf::Empty unused;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteSecurityPolicy, del, &unused,
      adapter.rpc_deadline());
  dash_opi::LogXlate("DeleteSecurityPolicy name=" + *name +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().Unbind(dash_acl_group_id);
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_create_dash_acl_rule(
    sai_object_id_t* dash_acl_rule_id,
    sai_object_id_t switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  if (dash_acl_rule_id == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  *dash_acl_rule_id = SAI_NULL_OBJECT_ID;

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  sai_object_id_t group_oid = SAI_NULL_OBJECT_ID;
  sai_dash_acl_rule_action_t action = SAI_DASH_ACL_RULE_ACTION_PERMIT;
  uint32_t priority = 0;
  sai_ip_prefix_t sip{};
  sai_ip_prefix_t dip{};
  bool have_sip = false;
  bool have_dip = false;
  int32_t proto = -1;
  uint16_t sport_lo = 0;
  uint16_t sport_hi = 65535;
  uint16_t dport_lo = 0;
  uint16_t dport_hi = 65535;
  bool have_sport = false;
  bool have_dport = false;

  for (uint32_t i = 0; i < attr_count; ++i) {
    switch (attr_list[i].id) {
      case SAI_DASH_ACL_RULE_ATTR_ACTION:
        action = static_cast<sai_dash_acl_rule_action_t>(attr_list[i].value.s32);
        break;
      case SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID:
        group_oid = attr_list[i].value.oid;
        break;
      case SAI_DASH_ACL_RULE_ATTR_PRIORITY:
        priority = attr_list[i].value.u32;
        break;
      case SAI_DASH_ACL_RULE_ATTR_SIP:
        have_sip = FirstPrefix(attr_list[i].value.ipprefixlist, &sip);
        break;
      case SAI_DASH_ACL_RULE_ATTR_DIP:
        have_dip = FirstPrefix(attr_list[i].value.ipprefixlist, &dip);
        break;
      case SAI_DASH_ACL_RULE_ATTR_PROTOCOL:
        if (attr_list[i].value.u8list.count > 0 &&
            attr_list[i].value.u8list.list != nullptr) {
          proto = attr_list[i].value.u8list.list[0];
        }
        break;
      case SAI_DASH_ACL_RULE_ATTR_SRC_PORT:
        if (attr_list[i].value.u16rangelist.count > 0 &&
            attr_list[i].value.u16rangelist.list != nullptr) {
          sport_lo = attr_list[i].value.u16rangelist.list[0].min;
          sport_hi = attr_list[i].value.u16rangelist.list[0].max;
          have_sport = true;
        }
        break;
      case SAI_DASH_ACL_RULE_ATTR_DST_PORT:
        if (attr_list[i].value.u16rangelist.count > 0 &&
            attr_list[i].value.u16rangelist.list != nullptr) {
          dport_lo = attr_list[i].value.u16rangelist.list[0].min;
          dport_hi = attr_list[i].value.u16rangelist.list[0].max;
          have_dport = true;
        }
        break;
      default:
        break;
    }
  }

  if (group_oid == SAI_NULL_OBJECT_ID) {
    return SAI_STATUS_MANDATORY_ATTRIBUTE_MISSING;
  }
  const auto policy_name = adapter.store().Name(group_oid);
  if (!policy_name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }

  const sai_object_id_t oid = adapter.store().Alloc(
      static_cast<sai_object_type_t>(SAI_OBJECT_TYPE_DASH_ACL_RULE), switch_id);
  const std::string id = HexId(oid);
  const std::string rule_name = "securityrules/" + id;

  cloud::CreateSecurityRuleRequest req;
  req.set_parent(adapter.opi_parent());
  req.set_securityrule_id(id);
  cloud::SecurityRule* rule = req.mutable_securityrule();
  rule->set_name(rule_name);
  cloud::SecurityRuleSpec* spec = rule->mutable_spec();
  spec->set_security_policy_name_ref(*policy_name);
  cloud::SecurityRuleAttrs* attrs = spec->mutable_attrs();
  attrs->set_priority(static_cast<int32_t>(priority));

  switch (action) {
    case SAI_DASH_ACL_RULE_ACTION_PERMIT:
      attrs->set_action(opinet::SECURITY_RULE_ACTION_ALLOW);
      attrs->set_terminating(true);
      break;
    case SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE:
      attrs->set_action(opinet::SECURITY_RULE_ACTION_ALLOW);
      attrs->set_terminating(false);
      break;
    case SAI_DASH_ACL_RULE_ACTION_DENY:
      attrs->set_action(opinet::SECURITY_RULE_ACTION_DENY);
      attrs->set_terminating(true);
      break;
    case SAI_DASH_ACL_RULE_ACTION_DENY_AND_CONTINUE:
      attrs->set_action(opinet::SECURITY_RULE_ACTION_DENY);
      attrs->set_terminating(false);
      break;
    default:
      attrs->set_action(opinet::SECURITY_RULE_ACTION_ALLOW);
      attrs->set_terminating(true);
      break;
  }

  opinet::RuleMatch* match = attrs->mutable_match();
  opinet::RuleL3Match* l3 = match->mutable_l3_match();
  if (proto >= 0) {
    l3->set_proto_num(proto);
  } else {
    l3->set_proto_wild_card(opinet::WILDCARD_MATCH_ANY);
  }
  if (have_sip) {
    if (!PackIpv4Prefix(sip, l3->mutable_src_prefix())) {
      return SAI_STATUS_INVALID_PARAMETER;
    }
  }
  if (have_dip) {
    if (!PackIpv4Prefix(dip, l3->mutable_dst_prefix())) {
      return SAI_STATUS_INVALID_PARAMETER;
    }
  }
  if (have_sport || have_dport) {
    opinet::PortMatch* ports = match->mutable_l4_match()->mutable_ports();
    if (have_sport) {
      ports->mutable_src_port_range()->set_port_low(sport_lo);
      ports->mutable_src_port_range()->set_port_high(sport_hi);
    }
    if (have_dport) {
      ports->mutable_dst_port_range()->set_port_low(dport_lo);
      ports->mutable_dst_port_range()->set_port_high(dport_hi);
    }
  }

  cloud::SecurityRule resp;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateSecurityRule, req, &resp,
      adapter.rpc_deadline());
  const std::string out_name = resp.name().empty() ? rule_name : resp.name();
  dash_opi::LogXlate("CreateSecurityRule name=" + out_name +
                     " policy=" + *policy_name +
                     " priority=" + std::to_string(priority) +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().Bind(oid, out_name);
  *dash_acl_rule_id = oid;
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_remove_dash_acl_rule(
    sai_object_id_t dash_acl_rule_id) {
  if (dash_acl_rule_id == SAI_NULL_OBJECT_ID) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }
  const auto name = adapter.store().Name(dash_acl_rule_id);
  if (!name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }
  cloud::DeleteSecurityRuleRequest del;
  del.set_name(*name);
  del.set_allow_missing(true);
  google::protobuf::Empty unused;
  const sai_status_t st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteSecurityRule, del, &unused,
      adapter.rpc_deadline());
  dash_opi::LogXlate("DeleteSecurityRule name=" + *name +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  adapter.store().Unbind(dash_acl_rule_id);
  return SAI_STATUS_SUCCESS;
}
