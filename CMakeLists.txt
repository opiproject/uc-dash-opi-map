# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
cmake_minimum_required(VERSION 3.16)
project(sai_opi_adapter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Protobuf REQUIRED)

# Distro packages (Ubuntu jammy libgrpc++-dev 1.30) ship pkg-config only.
# Upstream/source gRPC installs gRPCConfig.cmake. Accept either.
find_package(gRPC CONFIG QUIET)
if(gRPC_FOUND)
  message(STATUS "Found gRPC via CMake config")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
  add_library(gRPC::grpc++ ALIAS PkgConfig::GRPCXX)
  find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin REQUIRED)
  add_executable(gRPC::grpc_cpp_plugin IMPORTED)
  set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
    IMPORTED_LOCATION "${GRPC_CPP_PLUGIN_EXECUTABLE}")
  message(STATUS "Found gRPC via pkg-config (${GRPCXX_VERSION}); plugin ${GRPC_CPP_PLUGIN_EXECUTABLE}")
endif()

# ---------------------------------------------------------------------------
# Standard opiproject/opi-api + googleapis (google.api annotations).
# Override with -DOPI_API_ROOT=... -DGOOGLEAPIS_ROOT=... if already cloned.
# ---------------------------------------------------------------------------
set(OPI_API_ROOT "" CACHE PATH
    "Root of opiproject/opi-api (directory that contains network/cloud)")
set(GOOGLEAPIS_ROOT "" CACHE PATH
    "Root of googleapis (directory that contains google/api)")

include(FetchContent)

if(NOT OPI_API_ROOT)
  FetchContent_Declare(opi_api
    GIT_REPOSITORY https://github.com/opiproject/opi-api.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
  )
  FetchContent_GetProperties(opi_api)
  if(NOT opi_api_POPULATED)
    FetchContent_Populate(opi_api)
  endif()
  set(OPI_API_ROOT ${opi_api_SOURCE_DIR})
endif()

if(NOT GOOGLEAPIS_ROOT)
  FetchContent_Declare(googleapis
    GIT_REPOSITORY https://github.com/googleapis/googleapis.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_GetProperties(googleapis)
  if(NOT googleapis_POPULATED)
    FetchContent_Populate(googleapis)
  endif()
  set(GOOGLEAPIS_ROOT ${googleapis_SOURCE_DIR})
endif()

if(NOT EXISTS "${OPI_API_ROOT}/network/cloud/cloudrpc.proto")
  message(FATAL_ERROR
    "OPI_API_ROOT (${OPI_API_ROOT}) does not contain network/cloud/cloudrpc.proto")
endif()
if(NOT EXISTS "${OPI_API_ROOT}/inventory/inventory.proto")
  message(FATAL_ERROR
    "OPI_API_ROOT (${OPI_API_ROOT}) does not contain inventory/inventory.proto")
endif()

# ---------------------------------------------------------------------------
# protoc + grpc_cpp_plugin
# ---------------------------------------------------------------------------
if(TARGET protobuf::protoc)
  set(PROTOC_EXE $<TARGET_FILE:protobuf::protoc>)
else()
  set(PROTOC_EXE ${Protobuf_PROTOC_EXECUTABLE})
endif()
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

get_target_property(_pb_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
set(PROTOBUF_WELLKNOWN_INC ${_pb_inc})

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_IMPORT_ARGS
  -I ${OPI_API_ROOT}
  -I ${GOOGLEAPIS_ROOT}
)
foreach(_inc IN LISTS PROTOBUF_WELLKNOWN_INC)
  list(APPEND PROTO_IMPORT_ARGS -I ${_inc})
endforeach()

# Cloud + common messages. cloudrpc.proto is the CloudInfraService surface.
file(GLOB OPI_CLOUD_PROTOS CONFIGURE_DEPENDS
  "${OPI_API_ROOT}/network/cloud/*.proto")
file(GLOB OPI_COMMON_PROTOS CONFIGURE_DEPENDS
  "${OPI_API_ROOT}/network/opinetcommon/*.proto")

set(GOOGLE_API_PROTOS
  ${GOOGLEAPIS_ROOT}/google/api/annotations.proto
  ${GOOGLEAPIS_ROOT}/google/api/client.proto
  ${GOOGLEAPIS_ROOT}/google/api/field_behavior.proto
  ${GOOGLEAPIS_ROOT}/google/api/http.proto
  ${GOOGLEAPIS_ROOT}/google/api/launch_stage.proto
  ${GOOGLEAPIS_ROOT}/google/api/resource.proto
)

set(OPI_INV_PROTO ${OPI_API_ROOT}/inventory/inventory.proto)
set(OPI_PROTOS ${OPI_CLOUD_PROTOS} ${OPI_COMMON_PROTOS} ${OPI_INV_PROTO})

set(PROTO_CC)
set(PROTO_H)
foreach(_proto IN LISTS OPI_PROTOS GOOGLE_API_PROTOS)
  file(RELATIVE_PATH _rel ${OPI_API_ROOT} ${_proto})
  if(_rel MATCHES "^\\.\\.")
    file(RELATIVE_PATH _rel ${GOOGLEAPIS_ROOT} ${_proto})
  endif()
  get_filename_component(_rel_dir ${_rel} DIRECTORY)
  get_filename_component(_rel_we ${_rel} NAME_WE)
  set(_out_cc ${PROTO_GEN_DIR}/${_rel_dir}/${_rel_we}.pb.cc)
  set(_out_h  ${PROTO_GEN_DIR}/${_rel_dir}/${_rel_we}.pb.h)
  list(APPEND PROTO_CC ${_out_cc})
  list(APPEND PROTO_H ${_out_h})
endforeach()

add_custom_command(
  OUTPUT ${PROTO_CC} ${PROTO_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}
  COMMAND ${PROTOC_EXE}
          --cpp_out=${PROTO_GEN_DIR}
          ${PROTO_IMPORT_ARGS}
          ${OPI_PROTOS}
          ${GOOGLE_API_PROTOS}
  DEPENDS ${OPI_PROTOS} ${GOOGLE_API_PROTOS}
  COMMENT "Generating C++ protobufs from opi-api + google/api"
  VERBATIM
)

set(GRPC_CC ${PROTO_GEN_DIR}/network/cloud/cloudrpc.grpc.pb.cc)
set(GRPC_H  ${PROTO_GEN_DIR}/network/cloud/cloudrpc.grpc.pb.h)
add_custom_command(
  OUTPUT ${GRPC_CC} ${GRPC_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}/network/cloud
  COMMAND ${PROTOC_EXE}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          ${PROTO_IMPORT_ARGS}
          ${OPI_API_ROOT}/network/cloud/cloudrpc.proto
  DEPENDS ${OPI_API_ROOT}/network/cloud/cloudrpc.proto ${PROTO_H}
  COMMENT "Generating C++ gRPC client stubs (CloudInfraService)"
  VERBATIM
)

set(INV_GRPC_CC ${PROTO_GEN_DIR}/inventory/inventory.grpc.pb.cc)
set(INV_GRPC_H  ${PROTO_GEN_DIR}/inventory/inventory.grpc.pb.h)
add_custom_command(
  OUTPUT ${INV_GRPC_CC} ${INV_GRPC_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}/inventory
  COMMAND ${PROTOC_EXE}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          ${PROTO_IMPORT_ARGS}
          ${OPI_INV_PROTO}
  DEPENDS ${OPI_INV_PROTO} ${PROTO_H}
  COMMENT "Generating C++ gRPC client stubs (InventoryService)"
  VERBATIM
)

set(TEL_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/telemetry/v1/telemetry.proto)
set(TEL_CC ${PROTO_GEN_DIR}/telemetry/v1/telemetry.pb.cc)
set(TEL_H  ${PROTO_GEN_DIR}/telemetry/v1/telemetry.pb.h)
set(TEL_GRPC_CC ${PROTO_GEN_DIR}/telemetry/v1/telemetry.grpc.pb.cc)
set(TEL_GRPC_H  ${PROTO_GEN_DIR}/telemetry/v1/telemetry.grpc.pb.h)
add_custom_command(
  OUTPUT ${TEL_CC} ${TEL_H} ${TEL_GRPC_CC} ${TEL_GRPC_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}/telemetry/v1
  COMMAND ${PROTOC_EXE}
          --cpp_out=${PROTO_GEN_DIR}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
          ${TEL_PROTO}
  DEPENDS ${TEL_PROTO}
  COMMENT "Generating C++ gRPC client stubs (TelemetryService)"
  VERBATIM
)

set(NVME_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/storage/v1/frontend_nvme.proto)
set(IPSEC_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/security/v1/ipsec.proto)
set(NVME_CC ${PROTO_GEN_DIR}/storage/v1/frontend_nvme.pb.cc)
set(NVME_H  ${PROTO_GEN_DIR}/storage/v1/frontend_nvme.pb.h)
set(NVME_GRPC_CC ${PROTO_GEN_DIR}/storage/v1/frontend_nvme.grpc.pb.cc)
set(NVME_GRPC_H  ${PROTO_GEN_DIR}/storage/v1/frontend_nvme.grpc.pb.h)
set(IPSEC_CC ${PROTO_GEN_DIR}/security/v1/ipsec.pb.cc)
set(IPSEC_H  ${PROTO_GEN_DIR}/security/v1/ipsec.pb.h)
set(IPSEC_GRPC_CC ${PROTO_GEN_DIR}/security/v1/ipsec.grpc.pb.cc)
set(IPSEC_GRPC_H  ${PROTO_GEN_DIR}/security/v1/ipsec.grpc.pb.h)
set(SNAP_PROTO_IMPORT_ARGS -I ${CMAKE_CURRENT_SOURCE_DIR}/proto)
foreach(_inc IN LISTS PROTOBUF_WELLKNOWN_INC)
  list(APPEND SNAP_PROTO_IMPORT_ARGS -I ${_inc})
endforeach()
add_custom_command(
  OUTPUT ${NVME_CC} ${NVME_H} ${NVME_GRPC_CC} ${NVME_GRPC_H}
         ${IPSEC_CC} ${IPSEC_H} ${IPSEC_GRPC_CC} ${IPSEC_GRPC_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}/storage/v1
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}/security/v1
  COMMAND ${PROTOC_EXE}
          --cpp_out=${PROTO_GEN_DIR}
          --grpc_out=${PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
          ${SNAP_PROTO_IMPORT_ARGS}
          ${NVME_PROTO}
          ${IPSEC_PROTO}
  DEPENDS ${NVME_PROTO} ${IPSEC_PROTO}
  COMMENT "Generating C++ gRPC client stubs (FrontendNvmeService, IPsecService)"
  VERBATIM
)

# ---------------------------------------------------------------------------
# libsai_opi_adapter.so
#
# Host x86 .so talks OPI CloudInfra gRPC only. Do not link libdoca here —
# DOCA Flow / eSwitch lives on the BlueField SoC (dpu/nvidia/doca).
# DATAPATH=doca in config/lab.env is how adapter RPCs program eSwitch.
# ---------------------------------------------------------------------------
if(NOT DEFINED SAI_INCLUDE_DIR)
  set(SAI_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sai-stub)
endif()

add_library(sai_opi_adapter SHARED
  src/libsai_opi_adapter.cpp
  src/dash_opi/adapter.cc
  src/dash_opi/c_api.cc
  src/dash_opi/channel.cc
  src/dash_opi/inventory_client.cc
  src/dash_opi/object_store.cc
  src/dash_opi/status.cc
  src/sai_abi/sai_api.cc
  src/sai_abi/sai_create_acl.cc
  src/sai_abi/sai_create_eni.cc
  src/sai_abi/sai_create_route.cc
  src/sai_abi/sai_create_vnet.cc
  src/sai_abi/sai_get_eni_stats.cc
  src/sai_abi/sai_opi_snap.cc
  ${PROTO_CC}
  ${GRPC_CC}
  ${INV_GRPC_CC}
  ${TEL_CC}
  ${TEL_GRPC_CC}
  ${NVME_CC}
  ${NVME_GRPC_CC}
  ${IPSEC_CC}
  ${IPSEC_GRPC_CC}
)

set_target_properties(sai_opi_adapter PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "sai_opi_adapter"
  CXX_VISIBILITY_PRESET default
)

target_include_directories(sai_opi_adapter
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SAI_INCLUDE_DIR}/inc
    ${SAI_INCLUDE_DIR}/experimental
    ${PROTO_GEN_DIR}
)

target_link_libraries(sai_opi_adapter
  PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)

# Optional orchagent simulation (not part of the .so)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sai_create_eni.c)
  add_executable(test_sai_create_eni tests/test_sai_create_eni.c)
  target_link_libraries(test_sai_create_eni PRIVATE sai_opi_adapter)
  set_target_properties(test_sai_create_eni PROPERTIES LINKER_LANGUAGE CXX)
endif()
