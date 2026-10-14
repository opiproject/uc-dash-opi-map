// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package datapath

import (
	"fmt"
	"log"
	"strings"
)

// Backend names for Config.Backend. Default is OVS so the existing lab
// path does not require DOCA. "doca" is all-or-nothing: the same 8
// programming RPCs (Interface / Vnic / Route / SecurityRule) go to eSwitch
// Flow, never mixed with br-opi. Vpc / Subnet / RouteTable / SecurityPolicy
// stay in-memory on both backends.
const (
	BackendOVS  = "ovs"
	BackendDOCA = "doca"
	BackendAuto = "auto"
)

// flowEngine programs the eight Create/Delete RPCs that touch ports or
// flows. The object store on Datapath is shared.
type flowEngine interface {
	Name() string
	Start() error
	AttachPort(dev string) error
	ProgramOverlay(v *Vnic) error
	DeleteOverlay(v *Vnic) error
	ProgramRoute(r *Route, v *Vnic) error
	DeleteRoute(name string) error
	ProgramACL(r *SecurityRule, v *Vnic) error
	DeleteACL(name string) error
}

func normalizeBackend(s string) string {
	b := strings.ToLower(strings.TrimSpace(s))
	if b == "" {
		return BackendOVS
	}
	return b
}

func pickEngine(d *Datapath) (flowEngine, error) {
	b := normalizeBackend(d.cfg.Backend)
	switch b {
	case BackendOVS:
		return &ovsEngine{d: d}, nil
	case BackendDOCA:
		return newDOCAEngine(d), nil
	case BackendAuto:
		de := newDOCAEngine(d)
		if err := de.ping(); err != nil {
			log.Printf("datapath: auto: DOCA sidecar not ready (%v); using OVS", err)
			return &ovsEngine{d: d}, nil
		}
		log.Printf("datapath: auto selected DOCA (%s)", de.sock)
		return de, nil
	default:
		return nil, fmt.Errorf("unknown datapath backend %q (ovs|doca|auto)", d.cfg.Backend)
	}
}
