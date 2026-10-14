// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

package datapath

import "testing"

func TestPrefixHelpers(t *testing.T) {
	if !isAnyPrefix("") || !isAnyPrefix("0.0.0.0/0") {
		t.Fatal("default prefix should be any")
	}
	if isAnyPrefix("10.0.0.0/16") || isAnyPrefix("0.0.0.0/8") {
		t.Fatal("specific prefix must not be treated as any")
	}
	if prefixLen("10.0.0.0/16") != 16 {
		t.Fatalf("got %d", prefixLen("10.0.0.0/16"))
	}
	if routeOFPriority("10.0.0.0/16") != 316 {
		t.Fatalf("got %d", routeOFPriority("10.0.0.0/16"))
	}
}

func TestAclOFPriority(t *testing.T) {
	// Lower SAI priority wins → higher OpenFlow priority.
	if aclOFPriority(1) <= aclOFPriority(100) {
		t.Fatalf("sai pri 1 should outrank 100: %d vs %d", aclOFPriority(1), aclOFPriority(100))
	}
	if aclOFPriority(1) != 1399 {
		t.Fatalf("got %d", aclOFPriority(1))
	}
}

func TestRouteFlowMatch(t *testing.T) {
	got := routeFlowMatch("3", "10.0.0.0/16")
	want := "ip,in_port=3,nw_dst=10.0.0.0/16"
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}
	if routeFlowMatch("3", "0.0.0.0/0") != "ip,in_port=3" {
		t.Fatalf("default dest should omit nw_dst: %q", routeFlowMatch("3", "0.0.0.0/0"))
	}
}

func TestAclFlowMatchLabPermitHTTP(t *testing.T) {
	r := &SecurityRule{
		Proto:   6,
		Src:     "10.0.0.0/8",
		Dst:     "0.0.0.0/0",
		DstPort: 80,
	}
	got := aclFlowMatch("5", r)
	want := "tcp,in_port=5,nw_src=10.0.0.0/8,tp_dst=80"
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}
}

func TestNameCookieStable(t *testing.T) {
	a := nameCookie(cookieRouteGen, "routes/1")
	b := nameCookie(cookieRouteGen, "routes/1")
	c := nameCookie(cookieACLGen, "routes/1")
	if a != b {
		t.Fatal("cookie must be stable")
	}
	if a&cookieRouteGen != cookieRouteGen {
		t.Fatal("route cookie must keep generator bits")
	}
	if a == c {
		t.Fatal("route and ACL generators must not collide")
	}
}
