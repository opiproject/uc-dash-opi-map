#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sai.h>
#include <saiexperimentaldashoutboundrouting.h>

sai_status_t sai_create_outbound_routing_group(
    _Out_ sai_object_id_t* outbound_routing_group_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

sai_status_t sai_remove_outbound_routing_group(
    _In_ sai_object_id_t outbound_routing_group_id);

sai_status_t sai_create_outbound_routing_entry(
    _In_ const sai_outbound_routing_entry_t* outbound_routing_entry,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

sai_status_t sai_remove_outbound_routing_entry(
    _In_ const sai_outbound_routing_entry_t* outbound_routing_entry);

#ifdef __cplusplus
}
#endif
