#include "sai_abi/sai_create_acl.h"
#include "sai_abi/sai_create_eni.h"
#include "sai_abi/sai_create_route.h"
#include "sai_abi/sai_create_vnet.h"

#include "dash_opi/adapter.h"
#include "dash_opi/log.h"

#include <string>

extern "C" {
#include <sai.h>
#include <saiextensions.h>
#include <saiexperimentaldashacl.h>
#include <saiexperimentaldasheni.h>
#include <saiexperimentaldashoutboundrouting.h>
#include <saiexperimentaldashvnet.h>
}

namespace {

sai_dash_eni_api_t g_dash_eni_api{};
sai_dash_vnet_api_t g_dash_vnet_api{};
sai_dash_outbound_routing_api_t g_dash_outbound_routing_api{};
sai_dash_acl_api_t g_dash_acl_api{};
bool g_initialized = false;

}  // namespace

extern "C" sai_status_t sai_api_initialize(
    uint64_t flags,
    const sai_service_method_table_t* services) {
  (void)flags;
  if (g_initialized) {
    return SAI_STATUS_SUCCESS;
  }

  g_dash_eni_api = {};
  g_dash_eni_api.create_eni = sai_create_eni;
  g_dash_eni_api.remove_eni = sai_remove_eni;
  g_dash_eni_api.get_eni_stats = sai_get_eni_stats;

  g_dash_vnet_api = {};
  g_dash_vnet_api.create_vnet = sai_create_vnet;
  g_dash_vnet_api.remove_vnet = sai_remove_vnet;

  g_dash_outbound_routing_api = {};
  g_dash_outbound_routing_api.create_outbound_routing_group =
      sai_create_outbound_routing_group;
  g_dash_outbound_routing_api.remove_outbound_routing_group =
      sai_remove_outbound_routing_group;
  g_dash_outbound_routing_api.create_outbound_routing_entry =
      sai_create_outbound_routing_entry;
  g_dash_outbound_routing_api.remove_outbound_routing_entry =
      sai_remove_outbound_routing_entry;

  g_dash_acl_api = {};
  g_dash_acl_api.create_dash_acl_group = sai_create_dash_acl_group;
  g_dash_acl_api.remove_dash_acl_group = sai_remove_dash_acl_group;
  g_dash_acl_api.create_dash_acl_rule = sai_create_dash_acl_rule;
  g_dash_acl_api.remove_dash_acl_rule = sai_remove_dash_acl_rule;

  const sai_status_t st =
      dash_opi::Adapter::Instance().ConnectFromConfig(services);
  dash_opi::LogXlate(std::string("sai_api_initialize cloudinfra=") +
                     dash_opi::Adapter::Instance().cloudinfra_target() +
                     " snap=" + dash_opi::Adapter::Instance().snap_target() +
                     " grpc_status=" + std::to_string(st));
  if (st != SAI_STATUS_SUCCESS) {
    return st;
  }
  g_initialized = true;
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_api_query(sai_api_t api, void** api_method_table) {
  if (api_method_table == nullptr) {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  if (!g_initialized) {
    return SAI_STATUS_UNINITIALIZED;
  }
  if (api == SAI_API_DASH_ENI) {
    *api_method_table = &g_dash_eni_api;
    return SAI_STATUS_SUCCESS;
  }
  if (api == SAI_API_DASH_VNET) {
    *api_method_table = &g_dash_vnet_api;
    return SAI_STATUS_SUCCESS;
  }
  if (api == SAI_API_DASH_OUTBOUND_ROUTING) {
    *api_method_table = &g_dash_outbound_routing_api;
    return SAI_STATUS_SUCCESS;
  }
  if (api == SAI_API_DASH_ACL) {
    *api_method_table = &g_dash_acl_api;
    return SAI_STATUS_SUCCESS;
  }
  *api_method_table = nullptr;
  return SAI_STATUS_NOT_IMPLEMENTED;
}

extern "C" sai_status_t sai_api_uninitialize(void) {
  dash_opi::Adapter::Instance().Shutdown();
  g_initialized = false;
  g_dash_eni_api = {};
  g_dash_vnet_api = {};
  g_dash_outbound_routing_api = {};
  g_dash_acl_api = {};
  return SAI_STATUS_SUCCESS;
}

extern "C" sai_status_t sai_log_set(sai_api_t api, sai_log_level_t log_level) {
  (void)api;
  (void)log_level;
  return SAI_STATUS_SUCCESS;
}
