#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sai.h>
#include <saiexperimentaldasheni.h>

#ifndef SAI_ENI_ATTR_VM_MAC
#define SAI_ENI_ATTR_VM_MAC SAI_ENI_ATTR_CUSTOM_RANGE_START
#endif

/**
 * Open insecure gRPC channels: CloudInfra/Telemetry at target (typically
 * :50051) and SNAP inventory/NVMe/IPsec at OPI_SNAP_ADDRESS or sibling :50052.
 */
sai_status_t dash_opi_client_init(_In_ const char* target);

/** Same as dash_opi_client_init, with an explicit SNAP target (NULL = derive). */
sai_status_t dash_opi_client_init_ex(_In_ const char* cloudinfra,
                                     _In_ const char* snap);

void dash_opi_client_shutdown(void);

/**
 * Record the VNET → default subnet mapping that create_eni looks up from
 * SAI_ENI_ATTR_VNET_ID. Mirrors a prior successful create_vnet translation.
 */
sai_status_t dash_opi_bind_vnet(_In_ sai_object_id_t vnet_oid,
                                _In_ const char* vpc_name,
                                _In_ const char* subnet_name);

#ifdef __cplusplus
}
#endif
