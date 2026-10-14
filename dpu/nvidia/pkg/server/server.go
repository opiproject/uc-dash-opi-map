// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package server

import (
	"context"
	"fmt"
	"log"
	"strings"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/emptypb"

	cloud "dash-opi-map/dpu-cloudinfra/api/network/cloud"
	opinet "dash-opi-map/dpu-cloudinfra/api/network/opinetcommon"
	tel "dash-opi-map/dpu-cloudinfra/api/telemetry/v1"
	"dash-opi-map/dpu-cloudinfra/pkg/datapath"
)

// Server is CloudInfraService + TelemetryService.
type Server struct {
	cloud.UnimplementedCloudInfraServiceServer
	tel.UnimplementedTelemetryServiceServer
	dp *datapath.Datapath
}

func New(dp *datapath.Datapath) *Server {
	return &Server{dp: dp}
}

func (s *Server) CreateInterface(ctx context.Context, req *cloud.CreateInterfaceRequest) (*cloud.Interface, error) {
	_ = ctx
	iface := interfaceMsg(req)
	id := req.GetInterfaceId()
	name := ""
	if iface != nil {
		name = iface.GetName()
	}
	if name == "" && id != "" {
		name = "interfaces/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "interface name or interface_id is required")
	}
	var mac string
	var ifname string
	admin := true
	if iface != nil && iface.GetSpec() != nil {
		spec := iface.GetSpec()
		admin = spec.GetAdminStatus() != cloud.IfStatus_IF_STATUS_DOWN
		if h := spec.GetHostIfSpec(); h != nil {
			mac = datapath.FormatMAC(h.GetMacAddress())
			ifname = h.GetIfname()
		}
	}
	ifc, err := s.dp.CreateInterface(name, ifname, mac, admin)
	if err != nil {
		log.Printf("CreateInterface: %v", err)
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	out := &cloud.Interface{Name: name}
	out.Spec = &cloud.InterfaceSpec{
		AdminStatus: cloud.IfStatus_IF_STATUS_UP,
		Ifinfo:      &cloud.InterfaceSpec_HostIfSpec{HostIfSpec: &cloud.HostIfSpec{Ifname: ifc.Dev}},
	}
	if !admin {
		out.Spec.AdminStatus = cloud.IfStatus_IF_STATUS_DOWN
	}
	if mac != "" {
		out.Spec.GetHostIfSpec().MacAddress = iface.GetSpec().GetHostIfSpec().GetMacAddress()
	}
	st := cloud.IfStatus_IF_STATUS_UP
	if !admin {
		st = cloud.IfStatus_IF_STATUS_DOWN
	}
	out.Status = &cloud.InterfaceStatus{IfIndex: ifc.IfIdx, OperStatus: st}
	return out, nil
}

func (s *Server) DeleteInterface(ctx context.Context, req *cloud.DeleteInterfaceRequest) (*emptypb.Empty, error) {
	_ = ctx
	if err := s.dp.DeleteInterface(req.GetName(), req.GetAllowMissing()); err != nil {
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.Internal, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) CreateVnic(ctx context.Context, req *cloud.CreateVnicRequest) (*cloud.Vnic, error) {
	_ = ctx
	v := req.GetVnic()
	id := req.GetVnicId()
	name := ""
	if v != nil {
		name = v.GetName()
	}
	if name == "" && id != "" {
		name = "vnic/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "vnic name or vnic_id is required")
	}
	var hostRef, mac string
	var vni uint32
	if v != nil && v.GetSpec() != nil {
		spec := v.GetSpec()
		hostRef = spec.GetHostIfNameRef()
		mac = datapath.FormatMAC(spec.GetMacAddress())
		if enc := spec.GetFabricEncap(); enc != nil {
			if enc.GetType() == opinet.EncapType_ENCAP_TYPE_VXLAN || enc.GetValue() != nil {
				if val := enc.GetValue(); val != nil {
					vni = uint32(val.GetVnid())
				}
			}
		}
	}
	if _, err := s.dp.CreateVnic(name, hostRef, mac, vni); err != nil {
		log.Printf("CreateVnic: %v", err)
		code := codes.FailedPrecondition
		if strings.Contains(err.Error(), "not created") || strings.Contains(err.Error(), "required") {
			code = codes.FailedPrecondition
		}
		return nil, status.Errorf(code, "%v", err)
	}
	out := &cloud.Vnic{Name: name}
	if v != nil && v.GetSpec() != nil {
		out.Spec = v.GetSpec()
	}
	return out, nil
}

func (s *Server) DeleteVnic(ctx context.Context, req *cloud.DeleteVnicRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteVnic(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteVnic: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.Internal, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) CreateVpc(ctx context.Context, req *cloud.CreateVpcRequest) (*cloud.Vpc, error) {
	_ = ctx
	v := req.GetVpc()
	id := req.GetVpcId()
	name := ""
	if v != nil {
		name = v.GetName()
	}
	if name == "" && id != "" {
		name = "vpcs/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "vpc name or vpc_id is required")
	}
	var vni uint32
	if v != nil && v.GetSpec() != nil {
		if enc := v.GetSpec().GetFabricEncap(); enc != nil {
			if val := enc.GetValue(); val != nil {
				vni = uint32(val.GetVnid())
			}
		}
	}
	if _, err := s.dp.CreateVpc(name, vni); err != nil {
		log.Printf("CreateVpc: %v", err)
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	out := &cloud.Vpc{Name: name}
	if v != nil && v.GetSpec() != nil {
		out.Spec = v.GetSpec()
	} else {
		out.Spec = &cloud.VpcSpec{Type: cloud.VPCType_VPC_TYPE_TENANT}
	}
	return out, nil
}

func (s *Server) DeleteVpc(ctx context.Context, req *cloud.DeleteVpcRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteVpc(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteVpc: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) CreateSubnet(ctx context.Context, req *cloud.CreateSubnetRequest) (*cloud.Subnet, error) {
	_ = ctx
	sub := req.GetSubnet()
	id := req.GetSubnetId()
	name := ""
	if sub != nil {
		name = sub.GetName()
	}
	if name == "" && id != "" {
		name = "subnets/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "subnet name or subnet_id is required")
	}
	var vpcRef string
	var vni uint32
	if sub != nil && sub.GetSpec() != nil {
		spec := sub.GetSpec()
		vpcRef = spec.GetVpcNameRef()
		if enc := spec.GetFabricEncap(); enc != nil {
			if val := enc.GetValue(); val != nil {
				vni = uint32(val.GetVnid())
			}
		}
	}
	if _, err := s.dp.CreateSubnet(name, vpcRef, vni); err != nil {
		log.Printf("CreateSubnet: %v", err)
		code := codes.FailedPrecondition
		if strings.Contains(err.Error(), "not created") || strings.Contains(err.Error(), "required") {
			code = codes.FailedPrecondition
		}
		return nil, status.Errorf(code, "%v", err)
	}
	out := &cloud.Subnet{Name: name}
	if sub != nil && sub.GetSpec() != nil {
		out.Spec = sub.GetSpec()
	}
	return out, nil
}

func (s *Server) DeleteSubnet(ctx context.Context, req *cloud.DeleteSubnetRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteSubnet(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteSubnet: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.Internal, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) CreateRouteTable(ctx context.Context, req *cloud.CreateRouteTableRequest) (*cloud.RouteTable, error) {
	_ = ctx
	t := req.GetRoutetable()
	id := req.GetRoutetableId()
	name := ""
	if t != nil {
		name = t.GetName()
	}
	if name == "" && id != "" {
		name = "routetables/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "route_table name or routetable_id is required")
	}
	var af int32
	if t != nil && t.GetSpec() != nil {
		af = int32(t.GetSpec().GetAf())
	}
	if _, err := s.dp.CreateRouteTable(name, af); err != nil {
		log.Printf("CreateRouteTable: %v", err)
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	out := &cloud.RouteTable{Name: name}
	if t != nil && t.GetSpec() != nil {
		out.Spec = t.GetSpec()
	} else {
		out.Spec = &cloud.RouteTableSpec{Af: opinet.IpAf_IP_AF_INET}
	}
	return out, nil
}

func (s *Server) DeleteRouteTable(ctx context.Context, req *cloud.DeleteRouteTableRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteRouteTable(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteRouteTable: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) CreateRoute(ctx context.Context, req *cloud.CreateRouteRequest) (*cloud.Route, error) {
	_ = ctx
	r := req.GetRoute()
	id := req.GetRouteId()
	name := ""
	if r != nil {
		name = r.GetName()
	}
	if name == "" && id != "" {
		name = "routes/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "route name or route_id is required")
	}
	var tableRef, prefix, vpcRef string
	var class int32
	if r != nil && r.GetSpec() != nil {
		spec := r.GetSpec()
		tableRef = spec.GetRouteTableNameRef()
		if attrs := spec.GetAttrs(); attrs != nil {
			class = int32(attrs.GetRouteClass())
			if pfx := attrs.GetPrefix(); pfx != nil && pfx.GetAddr() != nil {
				addr := pfx.GetAddr()
				if addr.GetAf() == opinet.IpAf_IP_AF_INET {
					v := addr.GetV4Addr()
					prefix = fmt.Sprintf("%d.%d.%d.%d/%d",
						(v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff, pfx.GetLen())
				}
			}
			if nh := attrs.GetVpcNhInfo(); nh != nil {
				vpcRef = nh.GetVpcNameRef()
			}
		}
	}
	if _, err := s.dp.CreateRoute(name, tableRef, prefix, class, vpcRef); err != nil {
		log.Printf("CreateRoute: %v", err)
		code := codes.FailedPrecondition
		if strings.Contains(err.Error(), "not created") || strings.Contains(err.Error(), "required") {
			code = codes.FailedPrecondition
		}
		return nil, status.Errorf(code, "%v", err)
	}
	out := &cloud.Route{Name: name}
	if r != nil && r.GetSpec() != nil {
		out.Spec = r.GetSpec()
	}
	return out, nil
}

func (s *Server) DeleteRoute(ctx context.Context, req *cloud.DeleteRouteRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteRoute(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteRoute: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.Internal, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func prefixString(pfx *opinet.IPPrefix) string {
	if pfx == nil || pfx.GetAddr() == nil {
		return ""
	}
	addr := pfx.GetAddr()
	if addr.GetAf() != opinet.IpAf_IP_AF_INET {
		return ""
	}
	v := addr.GetV4Addr()
	return fmt.Sprintf("%d.%d.%d.%d/%d",
		(v>>24)&0xff, (v>>16)&0xff, (v>>8)&0xff, v&0xff, pfx.GetLen())
}

// exactL4Ports returns src/dst ports only when the range is a single value.
// OpenFlow flower cannot express a range without masked matches.
func exactL4Ports(l4 *opinet.RuleL4Match) (srcPort, dstPort int32) {
	if l4 == nil {
		return 0, 0
	}
	ports := l4.GetPorts()
	if ports == nil {
		return 0, 0
	}
	if s := ports.GetSrcPortRange(); s != nil && s.GetPortLow() > 0 && s.GetPortLow() == s.GetPortHigh() {
		srcPort = s.GetPortLow()
	}
	if d := ports.GetDstPortRange(); d != nil && d.GetPortLow() > 0 && d.GetPortLow() == d.GetPortHigh() {
		dstPort = d.GetPortLow()
	}
	return srcPort, dstPort
}

func (s *Server) CreateSecurityPolicy(ctx context.Context, req *cloud.CreateSecurityPolicyRequest) (*cloud.SecurityPolicy, error) {
	_ = ctx
	p := req.GetSecuritypolicy()
	id := req.GetSecuritypolicyId()
	name := ""
	if p != nil {
		name = p.GetName()
	}
	if name == "" && id != "" {
		name = "securitypolicys/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "securitypolicy name or securitypolicy_id is required")
	}
	var af int32
	if p != nil && p.GetSpec() != nil {
		af = int32(p.GetSpec().GetAddressFamily())
	}
	if _, err := s.dp.CreateSecurityPolicy(name, af); err != nil {
		log.Printf("CreateSecurityPolicy: %v", err)
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	out := &cloud.SecurityPolicy{Name: name}
	if p != nil && p.GetSpec() != nil {
		out.Spec = p.GetSpec()
	} else {
		out.Spec = &cloud.SecurityPolicySpec{
			Type:          cloud.SecurityPolicyType_SECURITY_POLICY_TYPE_TENANT,
			AddressFamily: opinet.IpAf_IP_AF_INET,
		}
	}
	return out, nil
}

func (s *Server) DeleteSecurityPolicy(ctx context.Context, req *cloud.DeleteSecurityPolicyRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteSecurityPolicy(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteSecurityPolicy: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.FailedPrecondition, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) CreateSecurityRule(ctx context.Context, req *cloud.CreateSecurityRuleRequest) (*cloud.SecurityRule, error) {
	_ = ctx
	r := req.GetSecurityrule()
	id := req.GetSecurityruleId()
	name := ""
	if r != nil {
		name = r.GetName()
	}
	if name == "" && id != "" {
		name = "securityrules/" + id
	}
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "securityrule name or securityrule_id is required")
	}
	var policyRef, src, dst string
	var priority, action, proto, srcPort, dstPort int32
	if r != nil && r.GetSpec() != nil {
		spec := r.GetSpec()
		policyRef = spec.GetSecurityPolicyNameRef()
		if attrs := spec.GetAttrs(); attrs != nil {
			priority = attrs.GetPriority()
			action = int32(attrs.GetAction())
			if m := attrs.GetMatch(); m != nil {
				if l3 := m.GetL3Match(); l3 != nil {
					proto = l3.GetProtoNum()
					src = prefixString(l3.GetSrcPrefix())
					dst = prefixString(l3.GetDstPrefix())
				}
				srcPort, dstPort = exactL4Ports(m.GetL4Match())
			}
		}
	}
	if _, err := s.dp.CreateSecurityRule(name, policyRef, priority, action, proto, src, dst, srcPort, dstPort); err != nil {
		log.Printf("CreateSecurityRule: %v", err)
		code := codes.FailedPrecondition
		if strings.Contains(err.Error(), "not created") || strings.Contains(err.Error(), "required") {
			code = codes.FailedPrecondition
		}
		return nil, status.Errorf(code, "%v", err)
	}
	out := &cloud.SecurityRule{Name: name}
	if r != nil && r.GetSpec() != nil {
		out.Spec = r.GetSpec()
	}
	return out, nil
}

func (s *Server) DeleteSecurityRule(ctx context.Context, req *cloud.DeleteSecurityRuleRequest) (*emptypb.Empty, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	if err := s.dp.DeleteSecurityRule(req.GetName(), req.GetAllowMissing()); err != nil {
		log.Printf("DeleteSecurityRule: %v", err)
		if strings.Contains(err.Error(), "not found") {
			return nil, status.Errorf(codes.NotFound, "%v", err)
		}
		return nil, status.Errorf(codes.Internal, "%v", err)
	}
	return &emptypb.Empty{}, nil
}

func (s *Server) GetVnicMetrics(ctx context.Context, req *tel.GetVnicMetricsRequest) (*tel.VnicMetrics, error) {
	_ = ctx
	if req.GetName() == "" {
		return nil, status.Error(codes.InvalidArgument, "name is required")
	}
	m, err := s.dp.GetMetrics(req.GetName())
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "%v", err)
	}
	return &tel.VnicMetrics{
		RxBytes:          m.RxBytes,
		RxPackets:        m.RxPackets,
		TxBytes:          m.TxBytes,
		TxPackets:        m.TxPackets,
		RxDrops:          m.RxDrops,
		TxDrops:          m.TxDrops,
		PolicyDrops:      m.PolicyDrops,
		ForwardingDrops:  m.ForwardingDrops,
	}, nil
}

func interfaceMsg(req *cloud.CreateInterfaceRequest) *cloud.Interface {
	if req == nil {
		return nil
	}
	m := req.ProtoReflect()
	fd := m.Descriptor().Fields().ByName("interface")
	if fd == nil {
		return nil
	}
	msg, ok := m.Get(fd).Message().Interface().(*cloud.Interface)
	if !ok {
		return nil
	}
	return msg
}
