#include "sai_abi/sai_create_eni.h"

#include "dash_opi/adapter.h"
#include "dash_opi/eni_attrs.h"
#include "dash_opi/grpc_bridge.h"
#include "dash_opi/log.h"

#include <network/cloud/cloudrpc.grpc.pb.h>
#include <network/cloud/interface.pb.h>
#include <network/cloud/vnic.pb.h>
#include <network/opinetcommon/networktypes.pb.h>

#include <google/protobuf/empty.pb.h>

#include <limits>
#include <sstream>
#include <string>

extern "C" {
#include <saiextensions.h>
}

namespace cloud = opi_api::network::cloud::v1alpha1;
namespace opinet = opi_api::network::opinetcommon::v1alpha1;

namespace {

std::string HexId(sai_object_id_t oid) {
  std::ostringstream os;
  os << std::hex << (oid & 0x0000ffffffffffffULL);
  return os.str();
}

int32_t SaturateU32ToI32(uint32_t v) {
  const auto max_i32 =
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
  return static_cast<int32_t>(v > max_i32 ? max_i32 : v);
}

template <typename Req>
auto MutableInterfaceField(Req* req, int) -> decltype(req->mutable_interface_()) {
  return req->mutable_interface_();
}
template <typename Req>
auto MutableInterfaceField(Req* req, long) -> decltype(req->mutable_interface()) {
  return req->mutable_interface();
}
template <typename Req>
auto MutableInterfaceField(Req* req) -> decltype(MutableInterfaceField(req, 0)) {
  return MutableInterfaceField(req, 0);
}

void PackHostInterface(const dash_opi::EniCreateAttrs& attrs,
                       const std::string& parent,
                       const std::string& if_id,
                       cloud::CreateInterfaceRequest* req) {
  req->set_parent(parent);
  req->set_interface_id(if_id);

  // Field `interface` is a C++ keyword. protoc 3.15+ emits interface_();
  // jammy protobuf 3.12 emits interface().
  cloud::Interface* iface = MutableInterfaceField(req);
  iface->set_name("interfaces/" + if_id);

  cloud::InterfaceSpec* spec = iface->mutable_spec();
  spec->set_admin_status(attrs.admin_state ? cloud::IF_STATUS_UP
                                           : cloud::IF_STATUS_DOWN);

  cloud::HostIfSpec* host = spec->mutable_host_if_spec();
  host->set_vf(false);
  if (attrs.has_vm_mac) {
    host->set_mac_address(dash_opi::MacToBytes(attrs.vm_mac));
  }
}

void PackVnic(const dash_opi::EniCreateAttrs& attrs,
              const std::string& parent,
              const std::string& vnic_id,
              const std::string& subnet_name,
              const std::string& host_if_name,
              cloud::CreateVnicRequest* req) {
  req->set_parent(parent);
  req->set_vnic_id(vnic_id);

  cloud::Vnic* vnic = req->mutable_vnic();
  vnic->set_name("vnic/" + vnic_id);

  cloud::VnicSpec* spec = vnic->mutable_spec();
  if (!subnet_name.empty()) {
    spec->set_subnet_name_ref(subnet_name);
  }
  if (attrs.has_cps) {
    spec->set_max_cps(SaturateU32ToI32(attrs.cps));
  }
  if (attrs.has_vm_mac) {
    spec->set_mac_address(dash_opi::MacToBytes(attrs.vm_mac));
  }
  if (attrs.has_vm_vni) {
    opinet::Encap* encap = spec->mutable_fabric_encap();
    encap->set_type(opinet::ENCAP_TYPE_VXLAN);
    encap->mutable_value()->set_vnid(static_cast<int32_t>(attrs.vm_vni));
  }
  if (!host_if_name.empty()) {
    spec->set_host_if_name_ref(host_if_name);
  }
}

sai_status_t RollbackInterface(cloud::CloudInfraService::Stub* stub,
                               const std::string& if_name,
                               std::chrono::milliseconds deadline) {
  cloud::DeleteInterfaceRequest del;
  del.set_name(if_name);
  del.set_allow_missing(true);
  google::protobuf::Empty unused;
  return dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteInterface, del, &unused,
      deadline);
}

}  // namespace

extern "C" sai_status_t sai_create_eni(
    sai_object_id_t* eni_id,
    sai_object_id_t switch_id,
    uint32_t attr_count,
    const sai_attribute_t* attr_list) {
  if (eni_id == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  *eni_id = SAI_NULL_OBJECT_ID;

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  const dash_opi::EniCreateAttrs attrs =
      dash_opi::ParseEniCreateAttrs(attr_count, attr_list);
  if (attrs.error_attr_index) {
    return SAI_STATUS_INVALID_PARAMETER;
  }

  std::string subnet_name;
  if (attrs.vnet_id != SAI_NULL_OBJECT_ID) {
    auto subnet = adapter.store().SubnetNameForVnet(attrs.vnet_id);
    if (!subnet) {
      return SAI_STATUS_ITEM_NOT_FOUND;
    }
    subnet_name = *subnet;
  }

  const sai_object_id_t oid =
      adapter.store().Alloc(SAI_OBJECT_TYPE_ENI, switch_id);
  const std::string id = HexId(oid);
  const auto deadline = adapter.rpc_deadline();
  const std::string& parent = adapter.opi_parent();

  // VM mode (DASH default): attach a host PF/VF Interface, then CreateVnic.
  // FNIC mode uses a tunnel ref instead; that path is not packed here.
  std::string host_if_name;
  if (attrs.mode == SAI_DASH_ENI_MODE_VM) {
    cloud::CreateInterfaceRequest if_req;
    PackHostInterface(attrs, parent, id, &if_req);

    cloud::Interface if_resp;
    const sai_status_t if_st = dash_opi::BlockingUnary(
        stub, &cloud::CloudInfraService::Stub::CreateInterface, if_req, &if_resp,
        deadline);
    host_if_name = if_resp.name().empty() ? "interfaces/" + id : if_resp.name();
    dash_opi::LogXlate(
        "CreateInterface name=" + host_if_name +
        " admin_status=" + (attrs.admin_state ? "UP" : "DOWN") +
        " mac=" + dash_opi::BytesToMac(dash_opi::MacToBytes(attrs.vm_mac)) +
        " grpc_status=" + std::to_string(if_st));
    if (if_st != SAI_STATUS_SUCCESS) {
      return if_st;
    }
  }

  cloud::CreateVnicRequest vnic_req;
  PackVnic(attrs, parent, id, subnet_name, host_if_name, &vnic_req);

  cloud::Vnic vnic_resp;
  const sai_status_t vnic_st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::CreateVnic, vnic_req, &vnic_resp,
      deadline);

  const std::string vnic_name =
      vnic_resp.name().empty() ? "vnic/" + id : vnic_resp.name();
  dash_opi::LogXlate(
      "CreateVnic name=" + vnic_name + " subnet_name_ref=" + subnet_name +
      " max_cps=" + std::to_string(attrs.cps) +
      " mac=" + dash_opi::BytesToMac(vnic_req.vnic().spec().mac_address()) +
      " vnid=" + std::to_string(attrs.vm_vni) +
      " host_if_name_ref=" + host_if_name +
      " grpc_status=" + std::to_string(vnic_st));

  if (vnic_st != SAI_STATUS_SUCCESS) {
    if (!host_if_name.empty()) {
      (void)RollbackInterface(stub, host_if_name, deadline);
    }
    return vnic_st;
  }

  adapter.store().Bind(oid, vnic_name);
  if (!host_if_name.empty()) {
    adapter.store().BindHostIf(oid, host_if_name);
  }
  *eni_id = oid;
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_remove_eni(sai_object_id_t eni_id) {
  if (eni_id == SAI_NULL_OBJECT_ID) {
    return SAI_STATUS_INVALID_PARAMETER;
  }

  auto& adapter = dash_opi::Adapter::Instance();
  cloud::CloudInfraService::Stub* stub = adapter.stub();
  if (stub == nullptr) {
    return SAI_STATUS_UNINITIALIZED;
  }

  const auto vnic_name = adapter.store().Name(eni_id);
  if (!vnic_name) {
    return SAI_STATUS_ITEM_NOT_FOUND;
  }

  const auto deadline = adapter.rpc_deadline();
  const auto host_if_name = adapter.store().HostIfName(eni_id);

  // Overlay first, then release the representor. allow_missing keeps a
  // retry after a partial delete (Vnic gone, Interface still allocated)
  // from failing the SAI remove.
  cloud::DeleteVnicRequest del_vnic;
  del_vnic.set_name(*vnic_name);
  del_vnic.set_allow_missing(true);
  google::protobuf::Empty unused;
  const sai_status_t vnic_st = dash_opi::BlockingUnary(
      stub, &cloud::CloudInfraService::Stub::DeleteVnic, del_vnic, &unused,
      deadline);
  dash_opi::LogXlate("DeleteVnic name=" + *vnic_name +
                     " grpc_status=" + std::to_string(vnic_st));
  if (vnic_st != SAI_STATUS_SUCCESS) {
    return vnic_st;
  }

  if (host_if_name) {
    const sai_status_t if_st =
        RollbackInterface(stub, *host_if_name, deadline);
    dash_opi::LogXlate("DeleteInterface name=" + *host_if_name +
                       " grpc_status=" + std::to_string(if_st));
    if (if_st != SAI_STATUS_SUCCESS) {
      return if_st;
    }
  }

  adapter.store().Unbind(eni_id);
  return SAI_STATUS_SUCCESS;
}
