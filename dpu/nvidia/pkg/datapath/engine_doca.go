// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package datapath

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"strings"
	"time"
)

const defaultDOCASocket = "/var/run/opi-doca.sock"

// docaEngine programs Interface / Vnic / Route / SecurityRule via the
// dpu_doca_flowd sidecar (DOCA Flow switch / eSwitch). The host adapter
// reaches this path through CloudInfra gRPC; this process does not link
// libdoca. DPDK EAL and hugepages live in the sidecar.
type docaEngine struct {
	d    *Datapath
	sock string
}

type docaReply struct {
	OK    bool   `json:"ok"`
	Error string `json:"error"`
}

func newDOCAEngine(d *Datapath) *docaEngine {
	sock := strings.TrimSpace(d.cfg.DOCASocket)
	if sock == "" {
		sock = os.Getenv("OPI_DOCA_SOCKET")
	}
	if sock == "" {
		sock = defaultDOCASocket
	}
	return &docaEngine{d: d, sock: sock}
}

func (e *docaEngine) Name() string { return BackendDOCA }

func (e *docaEngine) ping() error {
	return e.rpc(map[string]any{"op": "ping"})
}

func (e *docaEngine) Start() error {
	if e.d.cfg.DryRun {
		log.Printf("dry-run: DOCA engine (socket %s) — no eSwitch init", e.sock)
		return nil
	}
	if err := e.ping(); err != nil {
		return fmt.Errorf("DOCA sidecar %s: %w (on the DPU: make -C dpu/nvidia/doca DOCA=1 && ./dpu_doca_flowd)", e.sock, err)
	}
	req := map[string]any{
		"op":     "init",
		"uplink": e.d.cfg.Uplink,
		"vtep":   e.d.cfg.VTEPRemote,
	}
	if err := e.rpc(req); err != nil {
		return err
	}
	log.Printf("datapath: DOCA Flow/eSwitch via %s (uplink=%s) — not using br-opi", e.sock, e.d.cfg.Uplink)
	return nil
}

func (e *docaEngine) AttachPort(dev string) error {
	return e.rpc(map[string]any{"op": "attach", "dev": dev})
}

func (e *docaEngine) ProgramOverlay(v *Vnic) error {
	mac := ""
	if v.MAC != nil {
		mac = v.MAC.String()
	}
	return e.rpc(map[string]any{
		"op":     "overlay_add",
		"name":   v.Name,
		"dev":    v.HostDev,
		"vni":    v.VNI,
		"mac":    mac,
		"vtep":   e.d.cfg.VTEPRemote,
		"uplink": e.d.cfg.Uplink,
	})
}

func (e *docaEngine) DeleteOverlay(v *Vnic) error {
	return e.rpc(map[string]any{"op": "overlay_del", "name": v.Name, "vni": v.VNI})
}

func (e *docaEngine) ProgramRoute(r *Route, v *Vnic) error {
	vni := v.VNI
	if dest := e.d.lookupVpcLocked(r.VpcRef); dest != nil && dest.VNI != 0 {
		vni = dest.VNI
	}
	return e.rpc(map[string]any{
		"op":         "route_add",
		"name":       r.Name,
		"dev":        v.HostDev,
		"prefix":     r.Prefix,
		"prefix_len": prefixLen(r.Prefix),
		"class":      r.Class,
		"vni":        vni,
		"vtep":       e.d.cfg.VTEPRemote,
	})
}

func (e *docaEngine) DeleteRoute(name string) error {
	return e.rpc(map[string]any{"op": "route_del", "name": name})
}

func (e *docaEngine) ProgramACL(r *SecurityRule, v *Vnic) error {
	return e.rpc(map[string]any{
		"op":       "acl_add",
		"name":     r.Name,
		"dev":      v.HostDev,
		"priority": r.Priority,
		"action":   r.Action,
		"proto":    r.Proto,
		"src":      r.Src,
		"dst":      r.Dst,
		"sport":    r.SrcPort,
		"dport":    r.DstPort,
	})
}

func (e *docaEngine) DeleteACL(name string) error {
	return e.rpc(map[string]any{"op": "acl_del", "name": name})
}

func (e *docaEngine) rpc(req map[string]any) error {
	if e.d.cfg.DryRun {
		b, _ := json.Marshal(req)
		log.Printf("dry-run doca: %s", b)
		return nil
	}
	body, err := json.Marshal(req)
	if err != nil {
		return err
	}
	conn, err := net.DialTimeout("unix", e.sock, 2*time.Second)
	if err != nil {
		return err
	}
	defer conn.Close()
	deadline := 5 * time.Second
	if op, _ := req["op"].(string); op == "init" {
		deadline = 60 * time.Second
	}
	_ = conn.SetDeadline(time.Now().Add(deadline))
	if _, err := conn.Write(append(body, '\n')); err != nil {
		return err
	}
	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil {
		return err
	}
	var rep docaReply
	if err := json.Unmarshal([]byte(line), &rep); err != nil {
		return fmt.Errorf("doca reply: %w (%s)", err, strings.TrimSpace(line))
	}
	if !rep.OK {
		if rep.Error == "" {
			return fmt.Errorf("doca %s failed", req["op"])
		}
		return fmt.Errorf("doca: %s", rep.Error)
	}
	return nil
}
