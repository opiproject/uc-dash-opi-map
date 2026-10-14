#include "dash_opi/c_api.h"

#include "dash_opi/adapter.h"

#include <string>

extern "C" sai_status_t dash_opi_client_init(const char* target) {
  return dash_opi_client_init_ex(target, nullptr);
}

extern "C" sai_status_t dash_opi_client_init_ex(const char* cloudinfra,
                                                const char* snap) {
  if (cloudinfra == nullptr || cloudinfra[0] == '\0') {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  const std::string snap_s = (snap && snap[0] != '\0') ? snap : std::string();
  return dash_opi::Adapter::Instance().Connect(cloudinfra, snap_s);
}

extern "C" void dash_opi_client_shutdown(void) {
  dash_opi::Adapter::Instance().Shutdown();
}

extern "C" sai_status_t dash_opi_bind_vnet(sai_object_id_t vnet_oid,
                                           const char* vpc_name,
                                           const char* subnet_name) {
  if (vnet_oid == SAI_NULL_OBJECT_ID || vpc_name == nullptr ||
      subnet_name == nullptr || vpc_name[0] == '\0' ||
      subnet_name[0] == '\0') {
    return SAI_STATUS_INVALID_PARAMETER;
  }
  dash_opi::Adapter::Instance().store().BindVnet(
      vnet_oid, std::string(vpc_name), std::string(subnet_name));
  return SAI_STATUS_SUCCESS;
}
