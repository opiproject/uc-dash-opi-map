// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package datapath

import (
	"fmt"
	"log"
	"os/exec"
	"strconv"
)

// ovsEngine is the default no-DOCA programmer: br-opi OpenFlow 1.3
// (table 0 ACL, 1 L3, 2 overlay) with tc-flower fallback.
type ovsEngine struct {
	d *Datapath
}

func (e *ovsEngine) Name() string { return BackendOVS }

func (e *ovsEngine) Start() error {
	return e.d.ensureBridge()
}

func (e *ovsEngine) AttachPort(dev string) error {
	return e.d.addOVSPort(dev)
}

func (e *ovsEngine) ProgramOverlay(v *Vnic) error {
	return e.d.programOverlayOVSOrTC(v)
}

func (e *ovsEngine) DeleteOverlay(v *Vnic) error {
	return e.d.deleteOVSOverlayLocked(v)
}

func (e *ovsEngine) DeleteRoute(name string) error {
	e.d.unprogramCookieLocked(nameCookie(cookieRouteGen, name))
	return nil
}

func (e *ovsEngine) DeleteACL(name string) error {
	e.d.unprogramCookieLocked(nameCookie(cookieACLGen, name))
	return nil
}

func (e *ovsEngine) ProgramRoute(r *Route, v *Vnic) error {
	d := e.d
	if _, err := exec.LookPath("ovs-ofctl"); err != nil {
		return fmt.Errorf("ovs-ofctl missing; L3 needs OVS")
	}
	if d.cfg.DryRun {
		return nil
	}
	hostP, err := ofport(d.cfg.Bridge, v.HostDev)
	if err != nil {
		return err
	}
	actions, err := d.routeActionsLocked(r, v)
	if err != nil {
		return err
	}
	cookie := nameCookie(cookieRouteGen, r.Name)
	pri := routeOFPriority(r.Prefix)
	tbl := ""
	if d.of13 {
		tbl = "table=1,"
	}
	flow := fmt.Sprintf("cookie=0x%x,%spriority=%d,%s,actions=%s",
		cookie, tbl, pri, routeFlowMatch(hostP, r.Prefix), actions)
	_, err = d.ofctl("add-flow", d.cfg.Bridge, flow)
	return err
}

func (e *ovsEngine) ProgramACL(r *SecurityRule, v *Vnic) error {
	d := e.d
	if !d.of13 && r.Action != securityActionDeny {
		if r.Action == securityActionAllow {
			log.Printf("acl %s permit recorded (single-table default-allow)", r.Name)
		}
		d.programTCDenyLocked(r)
		return nil
	}
	if _, err := exec.LookPath("ovs-ofctl"); err != nil {
		d.programTCDenyLocked(r)
		return nil
	}
	if d.cfg.DryRun {
		return nil
	}
	hostP, err := ofport(d.cfg.Bridge, v.HostDev)
	if err != nil {
		return err
	}
	pri := aclOFPriority(r.Priority)
	tbl := ""
	if d.of13 {
		tbl = "table=0,"
	}
	actions := "drop"
	if r.Action != securityActionDeny {
		if !d.of13 {
			return nil
		}
		actions = "goto_table:1"
	}
	cookie := nameCookie(cookieACLGen, r.Name)
	flow := fmt.Sprintf("cookie=0x%x,%spriority=%d,%s,actions=%s",
		cookie, tbl, pri, aclFlowMatch(hostP, r), actions)
	if _, err := d.ofctl("add-flow", d.cfg.Bridge, flow); err != nil {
		if r.Action == securityActionDeny {
			d.programTCDenyOnDevLocked(r, v.HostDev)
		}
		return err
	}
	return nil
}

func (d *Datapath) programTCDenyOnDevLocked(r *SecurityRule, dev string) {
	if r.Action != securityActionDeny {
		return
	}
	pref := strconv.Itoa(aclOFPriority(r.Priority))
	_, _ = d.run("tc", "qdisc", "add", "dev", dev, "clsact")
	args := []string{"filter", "replace", "dev", dev, "egress", "pref", pref,
		"protocol", "ip", "flower"}
	if !isAnyPrefix(r.Src) {
		args = append(args, "src_ip", r.Src)
	}
	if !isAnyPrefix(r.Dst) {
		args = append(args, "dst_ip", r.Dst)
	}
	switch r.Proto {
	case 6:
		args = append(args, "ip_proto", "tcp")
	case 17:
		args = append(args, "ip_proto", "udp")
	case 1:
		args = append(args, "ip_proto", "icmp")
	}
	if (r.Proto == 6 || r.Proto == 17) && r.DstPort > 0 {
		args = append(args, "dst_port", strconv.Itoa(int(r.DstPort)))
	}
	args = append(args, "action", "drop")
	if _, err := d.run("tc", args...); err != nil {
		log.Printf("tc deny %s on %s: %v", r.Name, dev, err)
	}
}

func (d *Datapath) programOverlayOVSOrTC(v *Vnic) error {
	if _, err := exec.LookPath("ovs-ofctl"); err == nil {
		if err := d.programOVSLocked(v); err == nil {
			return nil
		} else {
			log.Printf("ovs overlay failed, trying tc-flower: %v", err)
		}
	}
	return d.programTCLocked(v)
}

func (d *Datapath) deleteOVSOverlayLocked(v *Vnic) error {
	if _, err := exec.LookPath("ovs-ofctl"); err == nil {
		cookie := fmt.Sprintf("0x%x", cookieBase|uint64(v.VNI))
		if _, err := d.ofctl("--strict", "del-flows", d.cfg.Bridge, "cookie="+cookie+"/-1"); err != nil {
			return err
		}
	}
	_, _ = d.run("tc", "filter", "del", "dev", v.HostDev, "egress", "pref", "200")
	return nil
}
