// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors

// Intel IPU Phase A: same CloudInfraService RPCs as NVIDIA, software OVS/tc
// datapath on ACC representors. Does not speak BridgePort / SNAP storage.
package main

import (
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"

	"google.golang.org/grpc"
	"google.golang.org/grpc/reflection"

	cloud "dash-opi-map/dpu-cloudinfra/api/network/cloud"
	tel "dash-opi-map/dpu-cloudinfra/api/telemetry/v1"
	"dash-opi-map/dpu-cloudinfra/pkg/datapath"
	"dash-opi-map/dpu-cloudinfra/pkg/server"
)

func main() {
	listen := flag.String("listen", ":50051", "gRPC listen address (host:port)")
	bridge := flag.String("ovs-bridge", "br-opi", "OVS bridge for overlay offload")
	uplink := flag.String("uplink", "", "uplink netdev (default: auto ens/enp/eth)")
	vtep := flag.String("vtep-remote", os.Getenv("OPI_VTEP_REMOTE"), "VXLAN tun_dst (optional)")
	dry := flag.Bool("dry-run", false, "log ip/ovs commands without executing")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("dpu-intel: ")

	dp, err := datapath.New(datapath.Config{
		Bridge:     *bridge,
		Uplink:     *uplink,
		VTEPRemote: *vtep,
		DryRun:     *dry,
		Vendor:     "intel",
	})
	if err != nil {
		log.Fatalf("datapath: %v", err)
	}

	lis, err := net.Listen("tcp", *listen)
	if err != nil {
		log.Fatalf("listen %s: %v", *listen, err)
	}

	gs := grpc.NewServer()
	svc := server.New(dp)
	cloud.RegisterCloudInfraServiceServer(gs, svc)
	tel.RegisterTelemetryServiceServer(gs, svc)
	reflection.Register(gs)

	go func() {
		ch := make(chan os.Signal, 1)
		signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
		<-ch
		log.Printf("stopping")
		gs.GracefulStop()
	}()

	log.Printf("CloudInfraService + TelemetryService on %s (vendor=intel bridge=%s uplink=%s)", lis.Addr(), *bridge, *uplink)
	if err := gs.Serve(lis); err != nil {
		fmt.Fprintf(os.Stderr, "serve: %v\n", err)
		os.Exit(1)
	}
}
