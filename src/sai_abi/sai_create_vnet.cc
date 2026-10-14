#include "sai_abi/sai_create_vnet.h"

#include "dash_opi/adapter.h"
#include "dash_opi/grpc_bridge.h"
#include "dash_opi/log.h"

#include <network/cloud/cloudrpc.grpc.pb.h>
#include <network/cloud/subnet.pb.h>
#include <network/cloud/vpc.pb.h>
#include <network/opinetcommon/networktypes.pb.h>

#include <google/protobuf/empty.pb.h>

#include <chrono>
#include <sstream>
#include <string>

extern "C" {
#include <saiextensions.h>
#include <saiexperimentaldashvnet.h>
}

namespace cloud = opi_api::network::cloud::v1alpha1;
namespace opinet = opi_api::network::opinetcommon::v1alpha1;

namespace {

std::string HexId(sai_object_id_t oid) {
  std::ostringstream os;
  os << std::hex << (oid & 0x0000ffffffffffffULL);
  return os.str();
}

uint32_t ParseVni(uint32_t attr_count, const sai_attribute_t* attr_list) {
  for (uint32_t i = 0; i < attr_count; ++i) {
    if (attr_list[i].id == SAI_VNET_ATTR_VNI) {
      return attr_list[i].value.u32;
    }
  }
  return 0;
}

sai_status_t RollbackVpc(cloud::CloudInfraService::Stub* stub,
                         const std::string& vpc_name,
                         std::chrono::milliseconds deadline) {
  cloud::DeleteVpcRequest del;
  del.set_name(vpc_name);
  del.set_allow_missing(true);
  google::protobuf::Empty unused;
  return dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteVpc, del, &unused, deadline);
}

}  // namespace

extern "C" sai_status_t sai_create_vnet(
    sai_object_id_t* vnet_id,
    sai_object_id_t switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  if (vnet_id == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  *vnet_id = SAI_NULL_OBJECT_ID;

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  const uint32_t vni = ParseVni(attr_count, attr_list);
  const sai_object_id_t oid =
      adapter.store().Alloc(static_cast<sai_object_type_t>(SAI_OBJECT_TYPE_VNET),
                            switch_id);
  const std::string id = HexId(oid);
  const auto deadline = adapter.rpc_deadline();
  const std::string& parent = adapter.opi_parent();
  const std::string vpc_name = "vpcs/" + id;
  const std::string subnet_name = "subnets/" + id + "-default";

  cloud::CreateVpcRequest vpc_req;
  vpc_req.set_parent(parent);
  vpc_req.set_vpc_id(id);
  cloud::Vpc* vpc = vpc_req.mutable_vpc();
  vpc->set_name(vpc_name);
  cloud::VpcSpec* vpc_spec = vpc->mutable_spec();
  vpc_spec->set_type(cloud::VPC_TYPE_TENANT);
  if (vni != 0) {
    opinet::Encap* encap = vpc_spec->mutable_fabric_encap();
    encap->set_type(opinet::ENCAP_TYPE_VXLAN);
    encap->mutable_value()->set_vnid(static_cast<int32_t>(vni));
  }

  cloud::Vpc vpc_resp;
  const sai_status_t vpc_st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateVpc, vpc_req, &vpc_resp,
      deadline);
  const std::string vpc_out =
      vpc_resp.name().empty() ? vpc_name : vpc_resp.name();
  dash_opi::LogXlate("CreateVpc name=" + vpc_out + " vni=" +
                     std::to_string(vni) +
                     " grpc_status=" + std::to_string(vpc_st));
  if (vpc_st != SAI_STATUS_SUCCESS) {
    return vpc_st;
  }

  cloud::CreateSubnetRequest sub_req;
  sub_req.set_parent(parent);
  sub_req.set_subnet_id(id + "-default");
  cloud::Subnet* subnet = sub_req.mutable_subnet();
  subnet->set_name(subnet_name);
  cloud::SubnetSpec* sub_spec = subnet->mutable_spec();
  sub_spec->set_vpc_name_ref(vpc_out);
  if (vni != 0) {
    opinet::Encap* encap = sub_spec->mutable_fabric_encap();
    encap->set_type(opinet::ENCAP_TYPE_VXLAN);
    encap->mutable_value()->set_vnid(static_cast<int32_t>(vni));
  }

  cloud::Subnet sub_resp;
  const sai_status_t sub_st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateSubnet, sub_req, &sub_resp,
      deadline);
  const std::string subnet_out =
      sub_resp.name().empty() ? subnet_name : sub_resp.name();
  dash_opi::LogXlate("CreateSubnet name=" + subnet_out +
                     " vpc_name_ref=" + vpc_out +
                     " grpc_status=" + std::to_string(sub_st));
  if (sub_st != SAI_STATUS_SUCCESS) {
    (void)RollbackVpc(stub, vpc_out, deadline);
    return sub_st;
  }

  adapter.store().BindVnet(oid, vpc_out, subnet_out);
  *vnet_id = oid;
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_remove_vnet(sai_object_id_t vnet_id) {
  if (vnet_id == SAI_NULL_OBJECT_ID) {
    return SAI_STATUS_INVALID_PARAMETER;
  }

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  const auto vpc_name = adapter.store().Name(vnet_id);
  if (!vpc_name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }
  const auto subnet_name = adapter.store().SubnetNameForVnet(vnet_id);
  const auto deadline = adapter.rpc_deadline();
  google::protobuf::Empty unused;

  if (subnet_name) {
    cloud::DeleteSubnetRequest del_sub;
    del_sub.set_name(*subnet_name);
    del_sub.set_allow_missing(true);
    const sai_status_t sub_st = dash_opi::BlockingUnary(
        stub, &cloud::CloudInfraService::Stub::DeleteSubnet, del_sub, &unused,
        deadline);
    dash_opi::LogXlate("DeleteSubnet name=" + *subnet_name +
                       " grpc_status=" + std::to_string(sub_st));
    if (sub_st != SAI_STATUS_SUCCESS) {
      return sub_st;
    }
  }

  cloud::DeleteVpcRequest del_vpc;
  del_vpc.set_name(*vpc_name);
  del_vpc.set_allow_missing(true);
  const sai_status_t vpc_st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteVpc, del_vpc, &unused,
      deadline);
  dash_opi::LogXlate("DeleteVpc name=" + *vpc_name +
                     " grpc_status=" + std::to_string(vpc_st));
  if (vpc_st != SAI_STATUS_SUCCESS) {
    return vpc_st;
  }

  adapter.store().Unbind(vnet_id);
  return SAI_STATUS_SUCCESS;
}
