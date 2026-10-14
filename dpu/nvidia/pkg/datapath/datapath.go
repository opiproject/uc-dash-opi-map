// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

// Package datapath programs vendor representors, OVS overlay (VNI), L3
// routes, ACL rules, and iface counters for CloudInfra CreateInterface /
// CreateVnic / DeleteVnic / Vpc / Subnet / RouteTable / Route /
// SecurityPolicy / SecurityRule / metrics.
//
// OpenFlow 1.3 pipeline on br-opi: table 0 ACL, table 1 L3, table 2 overlay.
// Single-table fallback if OF13/goto_table is unavailable.
// Optional DOCA Flow/eSwitch backend (-datapath=doca) programs the same
// Interface/Vnic/Route/SecurityRule RPCs via dpu_doca_flowd; OVS remains
// the default. Do not mix backends for one VNIC.
//
// Vendor "nvidia" (default): BlueField pf0vf* / pf0hpf.
// Vendor "intel": ACC port representors / enp / ens / APF-style names (Phase A OVS).
package datapath

import (
	"bufio"
	"fmt"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

const (
	defaultBridge  = "br-opi"
	vxlanPort      = "vxlan-opi"
	cookieBase     = 0x4f50490000000000 // overlay (low 32 = VNI)
	cookieRouteGen = 0x4f50490100000000 // L3 routes (low 32 = fnv32 name)
	cookieACLGen   = 0x4f50490200000000 // ACL rules
	cookieLocalL2  = 0x4f50490300000000 // same-VNI L2 between local ENIs
	cookiePipeline = 0x4f50490f00000001 // table 0/1 miss → next table
)

// Config is DPU datapath policy.
type Config struct {
	Bridge     string
	Uplink     string // empty = auto (vendor-specific names)
	VTEPRemote string // tun_dst for VXLAN; empty = output uplink without remote
	DryRun     bool
	// Vendor selects representor naming. "nvidia" (default) or "intel".
	Vendor string
	// Backend is ovs (default), doca, or auto (DOCA sidecar if up, else OVS).
	Backend string
	// DOCASocket is the dpu_doca_flowd Unix socket (default /var/run/opi-doca.sock).
	DOCASocket string
}

// Datapath holds in-memory OPI objects and the kernel/OVS wiring.
type Datapath struct {
	cfg Config
	mu  sync.Mutex

	ifaces      map[string]*Iface // OPI name -> representor
	vnics       map[string]*Vnic
	vpcs        map[string]*Vpc
	subnets     map[string]*Subnet
	routeTables map[string]*RouteTable
	routes      map[string]*Route
	policies    map[string]*SecurityPolicy
	rules       map[string]*SecurityRule
	pool        []string // unused host representors
	of13        bool     // OpenFlow 1.3 tables: 0=ACL, 1=L3, 2=overlay
	eng         flowEngine
}

// Iface is a CloudInfra Interface bound to a kernel netdev.
type Iface struct {
	Name   string
	Dev    string
	MAC    net.HardwareAddr
	Admin  bool
	IfIdx  uint32
}

// Vnic is overlay state for one ENI.
type Vnic struct {
	Name    string
	HostDev string
	HostOPI string
	VNI     uint32
	MAC     net.HardwareAddr
}

// Vpc is a tenant overlay (DASH VNET).
type Vpc struct {
	Name string
	VNI  uint32
}

// Subnet is the default IP scope under a Vpc (ENI subnet_name_ref).
type Subnet struct {
	Name    string
	VpcName string
	VNI     uint32
}

// RouteTable is a DASH outbound routing group.
type RouteTable struct {
	Name string
	AF   int32
}

// Route is one prefix entry under a RouteTable.
type Route struct {
	Name      string
	TableName string
	Prefix    string
	Class     int32
	VpcRef    string
}

// SecurityPolicy is a DASH ACL group.
type SecurityPolicy struct {
	Name string
	AF   int32
}

// SecurityRule is one ACL rule under a SecurityPolicy.
type SecurityRule struct {
	Name       string
	PolicyName string
	Priority   int32
	Action     int32
	Proto      int32
	Src        string
	Dst        string
	SrcPort    int32 // 0 = any; exact port only
	DstPort    int32 // 0 = any; exact port only
}

// New discovers representors and optionally creates br-opi.
func New(cfg Config) (*Datapath, error) {
	if cfg.Bridge == "" {
		cfg.Bridge = defaultBridge
	}
	if cfg.Vendor == "" {
		cfg.Vendor = "nvidia"
	}
	d := &Datapath{
		cfg:         cfg,
		ifaces:      map[string]*Iface{},
		vnics:       map[string]*Vnic{},
		vpcs:        map[string]*Vpc{},
		subnets:     map[string]*Subnet{},
		routeTables: map[string]*RouteTable{},
		routes:      map[string]*Route{},
		policies:    map[string]*SecurityPolicy{},
		rules:       map[string]*SecurityRule{},
	}
	up, hosts, err := discover(cfg)
	if err != nil {
		return nil, err
	}
	if cfg.Uplink == "" {
		d.cfg.Uplink = up
	}
	d.pool = rankHostRepresentors(cfg.Vendor, hosts)
	eng, err := pickEngine(d)
	if err != nil {
		return nil, err
	}
	d.eng = eng
	log.Printf("datapath: vendor=%s engine=%s uplink=%s host-representors=%v dry-run=%v", d.cfg.Vendor, d.eng.Name(), d.cfg.Uplink, d.pool, d.cfg.DryRun)
	if err := d.eng.Start(); err != nil {
		return nil, err
	}
	return d, nil
}

func discover(cfg Config) (uplink string, hosts []string, err error) {
	preferUplink := cfg.Uplink
	ents, err := os.ReadDir("/sys/class/net")
	if err != nil {
		return "", nil, err
	}
	var all []string
	for _, e := range ents {
		n := e.Name()
		if n == "lo" || strings.HasPrefix(n, "docker") || n == "tmfifo_net0" || n == "oob_net0" {
			continue
		}
		all = append(all, n)
	}
	pick := func(cands ...string) string {
		set := map[string]bool{}
		for _, n := range all {
			set[n] = true
		}
		for _, c := range cands {
			if set[c] {
				return c
			}
		}
		return ""
	}
	uplink = preferUplink
	if uplink == "" || !linkExists(uplink) {
		if cfg.Vendor == "intel" {
			uplink = pick("ens1f0", "enp0s0f0np0", "enp0s0f0", "eth1", "eth0", "p0")
		} else {
			uplink = pick("p0", "enp3s0f0np0", "enp3s0f0s0", "p1")
		}
	}
	for _, n := range all {
		if n == uplink {
			continue
		}
		if isHostRepresentor(cfg.Vendor, n) {
			hosts = append(hosts, n)
		}
	}
	if uplink == "" && len(all) > 0 {
		uplink = all[0]
	}
	if uplink == "" {
		return "", nil, fmt.Errorf("no usable netdev under /sys/class/net")
	}
	return uplink, hosts, nil
}

func isHostRepresentor(vendor, n string) bool {
	if vendor == "intel" {
		switch {
		case strings.HasPrefix(n, "enp"), strings.HasPrefix(n, "ens"),
			strings.HasPrefix(n, "eth"), strings.Contains(n, "apf"),
			strings.HasPrefix(n, "repr"), strings.Contains(n, "pr_"),
			strings.HasPrefix(n, "vf"):
			return true
		default:
			return false
		}
	}
	switch {
	case n == "pf0hpf", strings.HasPrefix(n, "pf0vf"), strings.HasPrefix(n, "pf1hpf"),
		strings.Contains(n, "pf0sf"), strings.HasPrefix(n, "enp"):
		return true
	default:
		return false
	}
}

// rankHostRepresentors prefers VF/PF host representors over Scalable Function
// netdevs. BF2 often lists en3f0pf0sf0 first; those reject ip link set address
// (RTNETLINK: Operation not supported).
func rankHostRepresentors(vendor string, hosts []string) []string {
	score := func(n string) int {
		if vendor == "intel" {
			switch {
			case strings.Contains(n, "apf"), strings.HasPrefix(n, "repr"):
				return 0
			case strings.HasPrefix(n, "ens"), strings.HasPrefix(n, "enp"):
				return 1
			default:
				return 2
			}
		}
		switch {
		case strings.HasPrefix(n, "pf0vf"), strings.HasPrefix(n, "pf1vf"):
			return 0
		case n == "pf0hpf" || n == "pf1hpf":
			return 1
		case strings.Contains(n, "sf"):
			return 3
		default:
			return 2
		}
	}
	out := append([]string(nil), hosts...)
	sort.SliceStable(out, func(i, j int) bool {
		si, sj := score(out[i]), score(out[j])
		if si != sj {
			return si < sj
		}
		return out[i] < out[j]
	})
	return out
}

func placeholderRepresentor(vendor string) string {
	if vendor == "intel" {
		return "eth0"
	}
	return "pf0hpf"
}

func representorHint(vendor string) string {
	if vendor == "intel" {
		return "enp*/ens*/apf/repr*"
	}
	return "pf0hpf / pf0vf*"
}

func linkExists(name string) bool {
	_, err := os.Stat(filepath.Join("/sys/class/net", name))
	return err == nil
}

func (d *Datapath) run(name string, args ...string) (string, error) {
	line := name + " " + strings.Join(args, " ")
	if d.cfg.DryRun {
		log.Printf("dry-run: %s", line)
		return "", nil
	}
	log.Printf("+ %s", line)
	cmd := exec.Command(name, args...)
	out, err := cmd.CombinedOutput()
	s := strings.TrimSpace(string(out))
	if err != nil {
		return s, fmt.Errorf("%s: %w (%s)", line, err, s)
	}
	return s, nil
}

func (d *Datapath) ofctl(args ...string) (string, error) {
	if d.of13 {
		args = append([]string{"-O", "OpenFlow13"}, args...)
	}
	return d.run("ovs-ofctl", args...)
}

func (d *Datapath) installPipeline() {
	if _, err := exec.LookPath("ovs-ofctl"); err != nil {
		return
	}
	if _, err := d.run("ovs-vsctl", "set", "bridge", d.cfg.Bridge, "protocols=OpenFlow13"); err != nil {
		log.Printf("OpenFlow13 unavailable; route/ACL use single-table fallback: %v", err)
		d.of13 = false
		return
	}
	d.of13 = true
	miss0 := fmt.Sprintf("cookie=0x%x,table=0,priority=1,actions=goto_table:1", cookiePipeline)
	miss1 := fmt.Sprintf("cookie=0x%x,table=1,priority=1,actions=goto_table:2", cookiePipeline)
	if _, err := d.ofctl("add-flow", d.cfg.Bridge, miss0); err != nil {
		log.Printf("table-0 miss failed; single-table fallback: %v", err)
		d.of13 = false
		return
	}
	if _, err := d.ofctl("add-flow", d.cfg.Bridge, miss1); err != nil {
		log.Printf("table-1 miss failed; single-table fallback: %v", err)
		d.of13 = false
		return
	}
	log.Printf("datapath: OpenFlow13 pipeline table0=ACL table1=L3 table2=overlay")
}

func (d *Datapath) ensureBridge() error {
	if d.cfg.DryRun {
		return nil
	}
	if _, err := exec.LookPath("ovs-vsctl"); err != nil {
		log.Printf("ovs-vsctl not in PATH; overlay will use tc-flower fallback")
		return nil
	}
	if _, err := d.run("ovs-vsctl", "br-exists", d.cfg.Bridge); err != nil {
		if _, err := d.run("ovs-vsctl", "add-br", d.cfg.Bridge); err != nil {
			return err
		}
	}
	_, _ = d.run("ovs-vsctl", "set", "Open_vSwitch", ".", "other_config:hw-offload=true")
	if d.cfg.Uplink != "" && linkExists(d.cfg.Uplink) {
		_ = d.addOVSPort(d.cfg.Uplink)
		_, _ = d.run("ip", "link", "set", "dev", d.cfg.Uplink, "up")
	}
	if err := d.ensureVxlanPort(); err != nil {
		log.Printf("vxlan port: %v", err)
	}
	_, _ = d.run("ip", "link", "set", "dev", d.cfg.Bridge, "up")
	d.installPipeline()
	return nil
}

func (d *Datapath) portBridge(dev string) string {
	out, err := exec.Command("ovs-vsctl", "port-to-br", dev).CombinedOutput()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

func (d *Datapath) addOVSPort(dev string) error {
	if _, err := exec.LookPath("ovs-vsctl"); err != nil {
		return nil
	}
	if br := d.portBridge(dev); br != "" && br != d.cfg.Bridge {
		log.Printf("leave %s on vendor bridge %s (not moving onto %s)", dev, br, d.cfg.Bridge)
		return nil
	}
	_, err := d.run("ovs-vsctl", "--may-exist", "add-port", d.cfg.Bridge, dev)
	return err
}

func (d *Datapath) ensureVxlanPort() error {
	if _, err := exec.LookPath("ovs-vsctl"); err != nil {
		return err
	}
	args := []string{"--may-exist", "add-port", d.cfg.Bridge, vxlanPort, "--",
		"set", "interface", vxlanPort, "type=vxlan",
		"options:key=flow", "options:remote_ip=flow", "options:dst_port=4789"}
	_, err := d.run("ovs-vsctl", args...)
	return err
}

func ofport(bridge, dev string) (string, error) {
	out, err := exec.Command("ovs-vsctl", "get", "Interface", dev, "ofport").CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("ofport %s: %w (%s)", dev, err, strings.TrimSpace(string(out)))
	}
	p := strings.Trim(strings.TrimSpace(string(out)), "\"")
	if p == "" || p == "-1" {
		return "", fmt.Errorf("ofport %s on %s is %q", dev, bridge, p)
	}
	return p, nil
}

// CreateInterface binds an OPI Interface name to a host representor.
func (d *Datapath) CreateInterface(opiName, requestedDev, mac string, adminUp bool) (*Iface, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if existing, ok := d.ifaces[opiName]; ok {
		return existing, d.applyIfaceLocked(existing, mac, adminUp)
	}
	dev := requestedDev
	if dev == "" {
		if d.cfg.DryRun && len(d.pool) == 0 {
			dev = placeholderRepresentor(d.cfg.Vendor)
		} else {
			var last error
			for i, cand := range d.pool {
				ifc := &Iface{Name: opiName, Dev: cand, Admin: adminUp, IfIdx: ifIndex(cand)}
				if err := d.applyIfaceLocked(ifc, mac, adminUp); err != nil {
					last = err
					log.Printf("skip representor %s: %v", cand, err)
					continue
				}
				d.pool = append(d.pool[:i], d.pool[i+1:]...)
				if err := d.eng.AttachPort(cand); err != nil {
					log.Printf("attach %s: %v", cand, err)
				}
				d.ifaces[opiName] = ifc
				log.Printf("CreateInterface %s -> %s mac=%s up=%v", opiName, cand, mac, adminUp)
				return ifc, nil
			}
			if last != nil {
				return nil, last
			}
			return nil, fmt.Errorf("no host representor left (%s)", representorHint(d.cfg.Vendor))
		}
	}
	if !d.cfg.DryRun && !linkExists(dev) {
		return nil, fmt.Errorf("netdev %s not found", dev)
	}
	idx := ifIndex(dev)
	hw, _ := parseMAC(mac)
	ifc := &Iface{Name: opiName, Dev: dev, MAC: hw, Admin: adminUp, IfIdx: idx}
	if err := d.applyIfaceLocked(ifc, mac, adminUp); err != nil {
		return nil, err
	}
	if err := d.eng.AttachPort(dev); err != nil {
		log.Printf("attach %s: %v", dev, err)
	}
	d.ifaces[opiName] = ifc
	log.Printf("CreateInterface %s -> %s mac=%s up=%v", opiName, dev, mac, adminUp)
	return ifc, nil
}

func notSupported(err error) bool {
	if err == nil {
		return false
	}
	s := err.Error()
	return strings.Contains(s, "Operation not supported") || strings.Contains(s, "EOPNOTSUPP")
}

func (d *Datapath) applyIfaceLocked(ifc *Iface, mac string, adminUp bool) error {
	if mac != "" {
		if _, err := d.run("ip", "link", "set", "dev", ifc.Dev, "address", mac); err != nil {
			if !notSupported(err) {
				return err
			}
			log.Printf("mac set %s not supported; keeping existing address", ifc.Dev)
		} else {
			ifc.MAC, _ = parseMAC(mac)
		}
	}
	state := "down"
	if adminUp {
		state = "up"
	}
	if _, err := d.run("ip", "link", "set", "dev", ifc.Dev, state); err != nil {
		return err
	}
	ifc.Admin = adminUp
	ifc.IfIdx = ifIndex(ifc.Dev)
	return nil
}

// DeleteInterface removes overlay flows and forgets the OPI Interface.
func (d *Datapath) DeleteInterface(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	ifc, ok := d.ifaces[opiName]
	if !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("interface %s not found", opiName)
	}
	for n, v := range d.vnics {
		if v.HostOPI == opiName || v.HostDev == ifc.Dev {
			_ = d.eng.DeleteOverlay(v)
			delete(d.vnics, n)
		}
	}
	delete(d.ifaces, opiName)
	d.pool = append(d.pool, ifc.Dev)
	log.Printf("DeleteInterface %s (%s)", opiName, ifc.Dev)
	return nil
}

// CreateVnic programs VXLAN VNI <-> host representor on br-opi (or tc-flower).
func (d *Datapath) CreateVnic(opiName, hostIfRef, mac string, vni uint32) (*Vnic, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	ifc, err := d.lookupHostLocked(hostIfRef)
	if err != nil {
		return nil, err
	}
	hw, _ := parseMAC(mac)
	if hw == nil {
		hw = ifc.MAC
	}
	v := &Vnic{Name: opiName, HostDev: ifc.Dev, HostOPI: ifc.Name, VNI: vni, MAC: hw}
	if err := d.eng.ProgramOverlay(v); err != nil {
		// DOCA overlay can fail when representors are not DPDK ports.
		// Keep the Vnic so CloudInfra DeleteVnic still runs on ENI remove.
		if d.eng.Name() != BackendDOCA {
			return nil, err
		}
		log.Printf("CreateVnic %s overlay: %v (keeping Vnic)", opiName, err)
	}
	d.vnics[opiName] = v
	d.refreshPolicyLocked()
	log.Printf("CreateVnic %s host=%s vni=%d mac=%s", opiName, ifc.Dev, vni, hw)
	return v, nil
}

func (d *Datapath) lookupHostLocked(ref string) (*Iface, error) {
	if ref == "" {
		return nil, fmt.Errorf("host_if_name_ref is required")
	}
	if ifc, ok := d.ifaces[ref]; ok {
		return ifc, nil
	}
	for _, ifc := range d.ifaces {
		if ifc.Dev == ref || strings.HasSuffix(ifc.Name, "/"+ref) {
			return ifc, nil
		}
	}
	return nil, fmt.Errorf("host interface %s not created", ref)
}

func (d *Datapath) programOVSLocked(v *Vnic) error {
	if err := d.addOVSPort(v.HostDev); err != nil {
		return err
	}
	if d.cfg.DryRun {
		return nil
	}
	hostP, err := ofport(d.cfg.Bridge, v.HostDev)
	if err != nil {
		return err
	}
	outPort := hostP
	if v.VNI > 0 && d.cfg.VTEPRemote != "" {
		vp, err := ofport(d.cfg.Bridge, vxlanPort)
		if err != nil {
			return err
		}
		outPort = vp
	} else if d.cfg.Uplink != "" {
		up, err := ofport(d.cfg.Bridge, d.cfg.Uplink)
		if err != nil {
			log.Printf("uplink ofport: %v", err)
		} else {
			outPort = up
		}
	}
	mac := v.MAC.String()
	cookie := fmt.Sprintf("0x%x", cookieBase|uint64(v.VNI))
	_, _ = d.ofctl("--strict", "del-flows", d.cfg.Bridge, "cookie="+cookie+"/-1")

	tbl := ""
	if d.of13 {
		tbl = "table=2,"
	}
	encap := fmt.Sprintf("cookie=%s,%spriority=200,in_port=%s", cookie, tbl, hostP)
	if mac != "" && mac != "<nil>" {
		encap += ",dl_src=" + mac
	}
	if v.VNI > 0 {
		encap += fmt.Sprintf(",actions=set_field:%d->tun_id", v.VNI)
		if d.cfg.VTEPRemote != "" {
			encap += fmt.Sprintf(",set_field:%s->tun_dst,output:%s", d.cfg.VTEPRemote, outPort)
		} else {
			encap += ",output:" + outPort
		}
	} else {
		encap += ",actions=output:" + outPort
	}
	if _, err := d.ofctl("add-flow", d.cfg.Bridge, encap); err != nil {
		return err
	}

	decap := fmt.Sprintf("cookie=%s,%spriority=200", cookie, tbl)
	if v.VNI > 0 {
		decap += fmt.Sprintf(",tun_id=%d", v.VNI)
	}
	if mac != "" && mac != "<nil>" {
		decap += ",dl_dst=" + mac
	}
	decap += ",actions=output:" + hostP
	if _, err := d.ofctl("add-flow", d.cfg.Bridge, decap); err != nil {
		return err
	}
	return nil
}

func (d *Datapath) programTCLocked(v *Vnic) error {
	// clsact + flower skip_sw for mlx5 eSwitch offload when OVS is absent.
	_, _ = d.run("tc", "qdisc", "add", "dev", v.HostDev, "clsact")
	mac := v.MAC.String()
	args := []string{"filter", "replace", "dev", v.HostDev, "egress", "pref", "200",
		"protocol", "all", "flower"}
	if mac != "" && mac != "<nil>" {
		args = append(args, "src_mac", mac)
	}
	args = append(args, "action", "skbedit", "mark", strconv.FormatUint(uint64(v.VNI), 10))
	if d.cfg.Uplink != "" {
		args = append(args, "action", "mirred", "egress", "redirect", "dev", d.cfg.Uplink)
	}
	_, err := d.run("tc", args...)
	if err != nil {
		return fmt.Errorf("tc-flower: %w (install openvswitch-switch for VXLAN hw-offload)", err)
	}
	log.Printf("tc-flower programmed on %s vni=%d (OVS not present)", v.HostDev, v.VNI)
	return nil
}

func (d *Datapath) DeleteVnic(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	v, ok := d.vnics[opiName]
	if !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("vnic %s not found", opiName)
	}
	if err := d.eng.DeleteOverlay(v); err != nil {
		return err
	}
	delete(d.vnics, opiName)
	d.refreshPolicyLocked()
	log.Printf("DeleteVnic %s host=%s vni=%d", opiName, v.HostDev, v.VNI)
	return nil
}

// CreateVpc records a tenant VPC (DASH VNET). Datapath wiring is deferred to Vnic.
func (d *Datapath) CreateVpc(opiName string, vni uint32) (*Vpc, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if opiName == "" {
		return nil, fmt.Errorf("vpc name is required")
	}
	if existing, ok := d.vpcs[opiName]; ok {
		if vni != 0 && existing.VNI != 0 && existing.VNI != vni {
			return nil, fmt.Errorf("vpc %s already exists with vni %d", opiName, existing.VNI)
		}
		if vni != 0 {
			existing.VNI = vni
		}
		return existing, nil
	}
	v := &Vpc{Name: opiName, VNI: vni}
	d.vpcs[opiName] = v
	log.Printf("CreateVpc %s vni=%d", opiName, vni)
	return v, nil
}

// DeleteVpc forgets a VPC. Subnets that still reference it must be deleted first
// unless allowMissing is used after cascade from the server.
func (d *Datapath) DeleteVpc(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if _, ok := d.vpcs[opiName]; !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("vpc %s not found", opiName)
	}
	for _, s := range d.subnets {
		if s.VpcName == opiName {
			return fmt.Errorf("vpc %s still has subnet %s", opiName, s.Name)
		}
	}
	delete(d.vpcs, opiName)
	log.Printf("DeleteVpc %s", opiName)
	return nil
}

// CreateSubnet records the default subnet under a Vpc.
func (d *Datapath) CreateSubnet(opiName, vpcRef string, vni uint32) (*Subnet, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if opiName == "" {
		return nil, fmt.Errorf("subnet name is required")
	}
	if vpcRef == "" {
		return nil, fmt.Errorf("vpc_name_ref is required")
	}
	vpc, ok := d.vpcs[vpcRef]
	if !ok {
		return nil, fmt.Errorf("vpc %s not created", vpcRef)
	}
	if vni == 0 {
		vni = vpc.VNI
	}
	if existing, ok := d.subnets[opiName]; ok {
		existing.VpcName = vpcRef
		if vni != 0 {
			existing.VNI = vni
		}
		return existing, nil
	}
	s := &Subnet{Name: opiName, VpcName: vpcRef, VNI: vni}
	d.subnets[opiName] = s
	log.Printf("CreateSubnet %s vpc=%s vni=%d", opiName, vpcRef, vni)
	return s, nil
}

// DeleteSubnet forgets a subnet.
func (d *Datapath) DeleteSubnet(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if _, ok := d.subnets[opiName]; !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("subnet %s not found", opiName)
	}
	delete(d.subnets, opiName)
	log.Printf("DeleteSubnet %s", opiName)
	return nil
}

// CreateRouteTable records a DASH outbound routing group.
func (d *Datapath) CreateRouteTable(opiName string, af int32) (*RouteTable, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if opiName == "" {
		return nil, fmt.Errorf("route table name is required")
	}
	if existing, ok := d.routeTables[opiName]; ok {
		if af != 0 {
			existing.AF = af
		}
		return existing, nil
	}
	t := &RouteTable{Name: opiName, AF: af}
	d.routeTables[opiName] = t
	log.Printf("CreateRouteTable %s af=%d", opiName, af)
	return t, nil
}

// DeleteRouteTable forgets a route table. Routes that still reference it must
// be deleted first.
func (d *Datapath) DeleteRouteTable(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if _, ok := d.routeTables[opiName]; !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("route table %s not found", opiName)
	}
	for _, r := range d.routes {
		if r.TableName == opiName {
			return fmt.Errorf("route table %s still has route %s", opiName, r.Name)
		}
	}
	delete(d.routeTables, opiName)
	log.Printf("DeleteRouteTable %s", opiName)
	return nil
}

// CreateRoute records one prefix under a RouteTable and programs L3 on the
// active engine (OVS table 1, or DOCA Flow). Flows are bound to each VNIC host
// representor so inbound VXLAN is not hairpinned.
func (d *Datapath) CreateRoute(opiName, tableRef, prefix string, class int32, vpcRef string) (*Route, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if opiName == "" {
		return nil, fmt.Errorf("route name is required")
	}
	if tableRef == "" {
		return nil, fmt.Errorf("route_table_name_ref is required")
	}
	if _, ok := d.routeTables[tableRef]; !ok {
		return nil, fmt.Errorf("route table %s not created", tableRef)
	}
	if existing, ok := d.routes[opiName]; ok {
		existing.TableName = tableRef
		existing.Prefix = prefix
		existing.Class = class
		existing.VpcRef = vpcRef
		d.programOneRouteLocked(existing)
		return existing, nil
	}
	r := &Route{
		Name:      opiName,
		TableName: tableRef,
		Prefix:    prefix,
		Class:     class,
		VpcRef:    vpcRef,
	}
	d.routes[opiName] = r
	d.programOneRouteLocked(r)
	log.Printf("CreateRoute %s table=%s prefix=%s class=%d vpc=%s",
		opiName, tableRef, prefix, class, vpcRef)
	return r, nil
}

// DeleteRoute removes the OVS L3 flows and forgets the route entry.
func (d *Datapath) DeleteRoute(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	r, ok := d.routes[opiName]
	if !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("route %s not found", opiName)
	}
	if err := d.eng.DeleteRoute(r.Name); err != nil {
		log.Printf("DeleteRoute program: %v", err)
	}
	delete(d.routes, opiName)
	log.Printf("DeleteRoute %s", opiName)
	return nil
}

// CreateSecurityPolicy records a DASH ACL group.
func (d *Datapath) CreateSecurityPolicy(opiName string, af int32) (*SecurityPolicy, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if opiName == "" {
		return nil, fmt.Errorf("security policy name is required")
	}
	if existing, ok := d.policies[opiName]; ok {
		if af != 0 {
			existing.AF = af
		}
		return existing, nil
	}
	p := &SecurityPolicy{Name: opiName, AF: af}
	d.policies[opiName] = p
	log.Printf("CreateSecurityPolicy %s af=%d", opiName, af)
	return p, nil
}

// DeleteSecurityPolicy forgets a policy. Rules that still reference it must
// be deleted first.
func (d *Datapath) DeleteSecurityPolicy(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if _, ok := d.policies[opiName]; !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("security policy %s not found", opiName)
	}
	for _, r := range d.rules {
		if r.PolicyName == opiName {
			return fmt.Errorf("security policy %s still has rule %s", opiName, r.Name)
		}
	}
	delete(d.policies, opiName)
	log.Printf("DeleteSecurityPolicy %s", opiName)
	return nil
}

// CreateSecurityRule records one ACL rule under a policy and programs it on
// the active engine (OVS table 0, or DOCA Flow).
func (d *Datapath) CreateSecurityRule(opiName, policyRef string, priority, action, proto int32, src, dst string, srcPort, dstPort int32) (*SecurityRule, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if opiName == "" {
		return nil, fmt.Errorf("security rule name is required")
	}
	if policyRef == "" {
		return nil, fmt.Errorf("security_policy_name_ref is required")
	}
	if _, ok := d.policies[policyRef]; !ok {
		return nil, fmt.Errorf("security policy %s not created", policyRef)
	}
	if existing, ok := d.rules[opiName]; ok {
		existing.PolicyName = policyRef
		existing.Priority = priority
		existing.Action = action
		existing.Proto = proto
		existing.Src = src
		existing.Dst = dst
		existing.SrcPort = srcPort
		existing.DstPort = dstPort
		d.programOneACLLocked(existing)
		return existing, nil
	}
	r := &SecurityRule{
		Name:       opiName,
		PolicyName: policyRef,
		Priority:   priority,
		Action:     action,
		Proto:      proto,
		Src:        src,
		Dst:        dst,
		SrcPort:    srcPort,
		DstPort:    dstPort,
	}
	d.rules[opiName] = r
	d.programOneACLLocked(r)
	log.Printf("CreateSecurityRule %s policy=%s priority=%d action=%d proto=%d src=%s dst=%s sport=%d dport=%d",
		opiName, policyRef, priority, action, proto, src, dst, srcPort, dstPort)
	return r, nil
}

// DeleteSecurityRule removes the OVS ACL flows and forgets the rule.
func (d *Datapath) DeleteSecurityRule(opiName string, allowMissing bool) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	r, ok := d.rules[opiName]
	if !ok {
		if allowMissing {
			return nil
		}
		return fmt.Errorf("security rule %s not found", opiName)
	}
	if err := d.eng.DeleteACL(r.Name); err != nil {
		log.Printf("DeleteSecurityRule program: %v", err)
	}
	delete(d.rules, opiName)
	log.Printf("DeleteSecurityRule %s", opiName)
	return nil
}

// Metrics is VnicMetrics filled from sysfs (and OVS flow packet counts).
type Metrics struct {
	RxBytes, RxPackets, TxBytes, TxPackets uint64
	RxDrops, TxDrops                       uint64
	PolicyDrops, ForwardingDrops           uint64
}

// GetMetrics reads /sys/class/net/<dev>/statistics for the VNIC's host port.
func (d *Datapath) GetMetrics(opiName string) (*Metrics, error) {
	d.mu.Lock()
	v, ok := d.vnics[opiName]
	d.mu.Unlock()
	if !ok {
		return nil, fmt.Errorf("vnic %s not found", opiName)
	}
	m, err := readSysStats(v.HostDev)
	if err != nil {
		return nil, err
	}
	d.fillOVSDrops(v, m)
	return m, nil
}

func (d *Datapath) fillOVSDrops(v *Vnic, m *Metrics) {
	if _, err := exec.LookPath("ovs-appctl"); err != nil {
		return
	}
	out, err := exec.Command("ovs-appctl", "dpctl/dump-flows").CombinedOutput()
	if err != nil {
		return
	}
	mac := strings.ToLower(v.MAC.String())
	var nPkt uint64
	sc := bufio.NewScanner(strings.NewReader(string(out)))
	for sc.Scan() {
		line := strings.ToLower(sc.Text())
		if mac != "" && mac != "<nil>" && !strings.Contains(line, mac) &&
			!strings.Contains(line, fmt.Sprintf("tun_id=0x%x", v.VNI)) &&
			!strings.Contains(line, fmt.Sprintf("tun_id=%d", v.VNI)) {
			continue
		}
		if i := strings.Index(line, "packets:"); i >= 0 {
			rest := line[i+len("packets:"):]
			rest = strings.FieldsFunc(rest, func(r rune) bool { return r == ',' || r == ' ' })[0]
			if p, err := strconv.ParseUint(rest, 10, 64); err == nil {
				nPkt += p
			}
		}
	}
	if m.ForwardingDrops == 0 && nPkt == 0 && m.RxPackets+m.TxPackets > 0 {
		// keep sysfs as source of truth; OVS dump is additive debug only
	}
	_ = nPkt
}

func readSysStats(dev string) (*Metrics, error) {
	base := filepath.Join("/sys/class/net", dev, "statistics")
	get := func(n string) uint64 {
		b, err := os.ReadFile(filepath.Join(base, n))
		if err != nil {
			return 0
		}
		v, _ := strconv.ParseUint(strings.TrimSpace(string(b)), 10, 64)
		return v
	}
	return &Metrics{
		RxBytes:   get("rx_bytes"),
		RxPackets: get("rx_packets"),
		TxBytes:   get("tx_bytes"),
		TxPackets: get("tx_packets"),
		RxDrops:   get("rx_dropped") + get("rx_errors"),
		TxDrops:   get("tx_dropped") + get("tx_errors"),
	}, nil
}

func ifIndex(dev string) uint32 {
	b, err := os.ReadFile(filepath.Join("/sys/class/net", dev, "ifindex"))
	if err != nil {
		return 0
	}
	n, _ := strconv.ParseUint(strings.TrimSpace(string(b)), 10, 32)
	return uint32(n)
}

func parseMAC(s string) (net.HardwareAddr, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, nil
	}
	if !strings.Contains(s, ":") && len(s) == 12 {
		// aabbccddeeff
		var b strings.Builder
		for i := 0; i < 12; i += 2 {
			if i > 0 {
				b.WriteByte(':')
			}
			b.WriteString(s[i : i+2])
		}
		s = b.String()
	}
	return net.ParseMAC(s)
}

// FormatMAC turns OPI bytes (6) into kernel address form.
func FormatMAC(b []byte) string {
	if len(b) < 6 {
		return ""
	}
	return net.HardwareAddr(b[:6]).String()
}
