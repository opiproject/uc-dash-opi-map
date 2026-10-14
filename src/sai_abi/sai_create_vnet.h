#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sai.h>
#include <saiexperimentaldashvnet.h>

/**
 * Create VNET — CreateVpc then CreateSubnet; binds ObjectStore for ENI lookup.
 * Signature matches sai_create_vnet_fn in saiexperimentaldashvnet.h.
 */
sai_status_t sai_create_vnet(
    _Out_ sai_object_id_t* vnet_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

/**
 * Remove VNET — DeleteSubnet then DeleteVpc.
 * Unknown oid returns SAI_STATUS_ITEM_NOT_FOUND.
 */
sai_status_t sai_remove_vnet(_In_ sai_object_id_t vnet_id);

#ifdef __cplusplus
}
#endif
