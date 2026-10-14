// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package datapath

import (
	"bufio"
	"encoding/json"
	"net"
	"path/filepath"
	"testing"
)

func TestNormalizeBackend(t *testing.T) {
	if normalizeBackend("") != BackendOVS {
		t.Fatal("empty backend must default to ovs")
	}
	if normalizeBackend("DOCA") != BackendDOCA {
		t.Fatal("case fold")
	}
}

func TestPickEngineOVS(t *testing.T) {
	d := &Datapath{cfg: Config{Backend: "ovs"}}
	eng, err := pickEngine(d)
	if err != nil {
		t.Fatal(err)
	}
	if eng.Name() != BackendOVS {
		t.Fatalf("got %s", eng.Name())
	}
}

func TestPickEngineUnknown(t *testing.T) {
	d := &Datapath{cfg: Config{Backend: "p4"}}
	if _, err := pickEngine(d); err == nil {
		t.Fatal("expected error")
	}
}

func TestDOCAEngineRPC(t *testing.T) {
	dir := t.TempDir()
	sock := filepath.Join(dir, "opi-doca.sock")
	ln, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		rd := bufio.NewReader(c)
		line, _ := rd.ReadString('\n')
		var req map[string]any
		_ = json.Unmarshal([]byte(line), &req)
		if req["op"] != "ping" {
			t.Errorf("op=%v", req["op"])
		}
		_, _ = c.Write([]byte(`{"ok":true}` + "\n"))
	}()

	d := &Datapath{cfg: Config{DOCASocket: sock}}
	e := newDOCAEngine(d)
	if err := e.ping(); err != nil {
		t.Fatal(err)
	}
}

func TestAutoFallsBackWithoutSocket(t *testing.T) {
	d := &Datapath{cfg: Config{Backend: "auto", DOCASocket: filepath.Join(t.TempDir(), "missing.sock")}}
	eng, err := pickEngine(d)
	if err != nil {
		t.Fatal(err)
	}
	if eng.Name() != BackendOVS {
		t.Fatalf("auto without sidecar must be ovs, got %s", eng.Name())
	}
}

func TestACLMatchStillLabHTTP(t *testing.T) {
	r := &SecurityRule{Proto: 6, Src: "10.0.0.0/8", Dst: "0.0.0.0/0", DstPort: 80}
	got := aclFlowMatch("5", r)
	if got != "tcp,in_port=5,nw_src=10.0.0.0/8,tp_dst=80" {
		t.Fatalf("got %q", got)
	}
}
