module dash-opi-map/dpu-intel

go 1.22

require (
	dash-opi-map/dpu-cloudinfra v0.0.0
	google.golang.org/grpc v1.64.1
	google.golang.org/protobuf v1.34.2
)

replace dash-opi-map/dpu-cloudinfra => ../nvidia
