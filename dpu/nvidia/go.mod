module dash-opi-map/dpu-cloudinfra

go 1.22

// NVIDIA BlueField SoC daemon. Module path kept so proto go_package
// (dash-opi-map/dpu-cloudinfra/api/...) does not churn. Intel Phase A
// imports this module via replace in dpu/intel.

require (
	google.golang.org/grpc v1.64.1
	google.golang.org/protobuf v1.34.2
)
