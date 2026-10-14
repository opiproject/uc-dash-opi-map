// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package datapath

import (
	"fmt"
	"hash/fnv"
	"log"
	"os/exec"
	"strconv"
	"strings"
)

// RouteClass values match proto/network/cloud/route.proto (avoid importing
// generated stubs from this package).
const (
	routeClassUnspecified = 0 // DASH DROP
	routeClassInternet    = 1
	routeClassService     = 2
	routeClassVPC         = 3
)

const (
	securityActionAllow = 1
	securityActionDeny  = 2
)

func nameCookie(base uint64, name string) uint64 {
	h := fnv.New32a()
	_, _ = h.Write([]byte(name))
	return base | uint64(h.Sum32())
}

func isAnyPrefix(p string) bool {
	if p == "" || p == "0.0.0.0" {
		return true
	}
	return p == "0.0.0.0/0"
}

func prefixLen(p string) int {
	_, n, ok := strings.Cut(p, "/")
	if !ok {
		if p == "" {
			return 0
		}
		return 32
	}
	v, err := strconv.Atoi(n)
	if err != nil || v < 0 {
		return 0
	}
	if v > 32 {
		return 32
	}
	return v
}

func routeOFPriority(prefix string) int {
	return 300 + prefixLen(prefix)
}

func aclOFPriority(saiPri int32) int {
	p := saiPri
	if p < 0 {
		p = 0
	}
	if p > 999 {
		p = 999
	}
	return 400 + int(1000-p)
}

func routeFlowMatch(inPort, prefix string) string {
	m := "ip,in_port=" + inPort
	if !isAnyPrefix(prefix) && prefix != "" {
		m += ",nw_dst=" + prefix
	}
	return m
}

func aclFlowMatch(inPort string, r *SecurityRule) string {
	protoName := "ip"
	switch r.Proto {
	case 6:
		protoName = "tcp"
	case 17:
		protoName = "udp"
	case 1:
		protoName = "icmp"
	}
	parts := []string{protoName, "in_port=" + inPort}
	if r.Proto != 0 && r.Proto != 6 && r.Proto != 17 && r.Proto != 1 {
		parts = append(parts, fmt.Sprintf("nw_proto=%d", r.Proto))
	}
	if !isAnyPrefix(r.Src) {
		parts = append(parts, "nw_src="+r.Src)
	}
	if !isAnyPrefix(r.Dst) {
		parts = append(parts, "nw_dst="+r.Dst)
	}
	if (r.Proto == 6 || r.Proto == 17) && r.SrcPort > 0 {
		parts = append(parts, fmt.Sprintf("tp_src=%d", r.SrcPort))
	}
	if (r.Proto == 6 || r.Proto == 17) && r.DstPort > 0 {
		parts = append(parts, fmt.Sprintf("tp_dst=%d", r.DstPort))
	}
	return strings.Join(parts, ",")
}

func (d *Datapath) lookupVpcLocked(ref string) *Vpc {
	if ref == "" {
		return nil
	}
	if v, ok := d.vpcs[ref]; ok {
		return v
	}
	for name, v := range d.vpcs {
		if strings.HasSuffix(name, "/"+ref) || strings.HasSuffix(ref, "/"+name) {
			return v
		}
	}
	return nil
}

func (d *Datapath) unprogramCookieLocked(cookie uint64) {
	if _, err := exec.LookPath("ovs-ofctl"); err != nil {
		return
	}
	spec := fmt.Sprintf("cookie=0x%x/-1", cookie)
	if _, err := d.ofctl("--strict", "del-flows", d.cfg.Bridge, spec); err != nil {
		log.Printf("del-flows %s: %v", spec, err)
	}
}

// refreshPolicyLocked re-installs every route and ACL on current VNICs.
// Called after CreateVnic / DeleteVnic so L3/ACL follow the representor.
func (d *Datapath) refreshPolicyLocked() {
	for _, r := range d.routes {
		d.programOneRouteLocked(r)
	}
	for _, r := range d.rules {
		d.programOneACLLocked(r)
	}
	d.programSameVniL2Locked()
}

// programSameVniL2Locked switches unicast + broadcast between ENIs that share
// a VNI on this DPU (priority 220/210, above overlay encap 200). Without
// this, east-west iperf on two host VFs only hits VXLAN/uplink and never
// the peer representor.
func (d *Datapath) programSameVniL2Locked() {
	if d.cfg.DryRun {
		return
	}
	if d.eng.Name() != BackendOVS {
		return
	}
	if _, err := exec.LookPath("ovs-ofctl"); err != nil {
		return
	}
	byVNI := map[uint32][]*Vnic{}
	for _, v := range d.vnics {
		byVNI[v.VNI] = append(byVNI[v.VNI], v)
	}
	tbl := ""
	if d.of13 {
		tbl = "table=2,"
	}
	for vni, vs := range byVNI {
		cookie := fmt.Sprintf("0x%x", cookieLocalL2|uint64(vni))
		_, _ = d.ofctl("--strict", "del-flows", d.cfg.Bridge, "cookie="+cookie+"/-1")
		if len(vs) < 2 {
			continue
		}
		type hop struct {
			port string
			mac  string
			dev  string
		}
		hops := make([]hop, 0, len(vs))
		ok := true
		for _, v := range vs {
			p, err := ofport(d.cfg.Bridge, v.HostDev)
			if err != nil {
				ok = false
				break
			}
			mac := ""
			if v.MAC != nil {
				mac = v.MAC.String()
			}
			hops = append(hops, hop{port: p, mac: mac, dev: v.HostDev})
		}
		if !ok {
			continue
		}
		for i, a := range hops {
			flood := make([]string, 0, len(hops)-1)
			for j, b := range hops {
				if i != j {
					flood = append(flood, "output:"+b.port)
				}
			}
			if len(flood) == 0 {
				continue
			}
			bcast := fmt.Sprintf("cookie=%s,%spriority=210,in_port=%s,dl_dst=ff:ff:ff:ff:ff:ff,actions=%s",
				cookie, tbl, a.port, strings.Join(flood, ","))
			if _, err := d.ofctl("add-flow", d.cfg.Bridge, bcast); err != nil {
				log.Printf("same-vni flood %s: %v", a.dev, err)
			}
			for j, b := range hops {
				if i == j || b.mac == "" || b.mac == "<nil>" {
					continue
				}
				uni := fmt.Sprintf("cookie=%s,%spriority=220,in_port=%s,dl_dst=%s,actions=output:%s",
					cookie, tbl, a.port, b.mac, b.port)
				if _, err := d.ofctl("add-flow", d.cfg.Bridge, uni); err != nil {
					log.Printf("same-vni l2 %s→%s: %v", a.dev, b.dev, err)
				}
			}
		}
		log.Printf("same-vni L2 vni=%d local-enis=%d", vni, len(hops))
	}
}

func (d *Datapath) programOneRouteLocked(r *Route) {
	if err := d.eng.DeleteRoute(r.Name); err != nil {
		log.Printf("unprogram route %s: %v", r.Name, err)
	}
	if r.Prefix == "" {
		log.Printf("skip route %s: empty prefix", r.Name)
		return
	}
	if d.cfg.DryRun {
		log.Printf("dry-run: route %s prefix=%s class=%d on %d vnics engine=%s",
			r.Name, r.Prefix, r.Class, len(d.vnics), d.eng.Name())
		return
	}
	if len(d.vnics) == 0 {
		log.Printf("route %s stored; datapath install waits for CreateVnic", r.Name)
		return
	}
	for _, v := range d.vnics {
		if err := d.eng.ProgramRoute(r, v); err != nil {
			log.Printf("route %s on %s: %v", r.Name, v.HostDev, err)
		}
	}
}

func (d *Datapath) routeActionsLocked(r *Route, v *Vnic) (string, error) {
	switch r.Class {
	case routeClassUnspecified:
		return "drop", nil
	case routeClassInternet:
		return d.outputUplinkLocked()
	case routeClassVPC, routeClassService:
		vni := v.VNI
		if dest := d.lookupVpcLocked(r.VpcRef); dest != nil && dest.VNI != 0 {
			vni = dest.VNI
		}
		return d.encapActionsLocked(vni)
	default:
		return d.encapActionsLocked(v.VNI)
	}
}

func (d *Datapath) outputUplinkLocked() (string, error) {
	if d.cfg.Uplink == "" {
		return "", fmt.Errorf("no uplink")
	}
	up, err := ofport(d.cfg.Bridge, d.cfg.Uplink)
	if err != nil {
		return "", err
	}
	return "output:" + up, nil
}

func (d *Datapath) encapActionsLocked(vni uint32) (string, error) {
	if vni > 0 && d.cfg.VTEPRemote != "" {
		vp, err := ofport(d.cfg.Bridge, vxlanPort)
		if err != nil {
			return "", err
		}
		return fmt.Sprintf("set_field:%d->tun_id,set_field:%s->tun_dst,output:%s",
			vni, d.cfg.VTEPRemote, vp), nil
	}
	if d.cfg.Uplink != "" {
		up, err := ofport(d.cfg.Bridge, d.cfg.Uplink)
		if err != nil {
			return "", err
		}
		if vni > 0 {
			return fmt.Sprintf("set_field:%d->tun_id,output:%s", vni, up), nil
		}
		return "output:" + up, nil
	}
	return "", fmt.Errorf("no VXLAN or uplink for encap")
}

func (d *Datapath) programOneACLLocked(r *SecurityRule) {
	if err := d.eng.DeleteACL(r.Name); err != nil {
		log.Printf("unprogram acl %s: %v", r.Name, err)
	}
	if d.cfg.DryRun {
		log.Printf("dry-run: acl %s action=%d proto=%d dport=%d on %d vnics engine=%s",
			r.Name, r.Action, r.Proto, r.DstPort, len(d.vnics), d.eng.Name())
		return
	}
	if len(d.vnics) == 0 {
		log.Printf("acl %s stored; datapath install waits for CreateVnic", r.Name)
		return
	}
	for _, v := range d.vnics {
		if err := d.eng.ProgramACL(r, v); err != nil {
			log.Printf("acl %s on %s: %v", r.Name, v.HostDev, err)
		}
	}
}

func (d *Datapath) programTCDenyLocked(r *SecurityRule) {
	if r.Action != securityActionDeny {
		return
	}
	for _, v := range d.vnics {
		d.programTCDenyOnDevLocked(r, v.HostDev)
	}
}
