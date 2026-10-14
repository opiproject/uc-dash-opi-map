// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
//
// Host SAI adapter: SONiC dashorch DASH C ABI → OPI CloudInfraService gRPC.
// Built as libsai_opi_adapter.so (see CMakeLists.txt).

#include "dash_opi/c_api.h"
#include "sai_abi/sai_create_eni.h"

extern "C" const char* sai_opi_adapter_version(void) {
  return "0.1.0";
}
