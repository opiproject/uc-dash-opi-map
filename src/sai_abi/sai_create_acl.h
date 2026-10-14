#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sai.h>
#include <saiexperimentaldashacl.h>

sai_status_t sai_create_dash_acl_group(
    _Out_ sai_object_id_t* dash_acl_group_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

sai_status_t sai_remove_dash_acl_group(_In_ sai_object_id_t dash_acl_group_id);

sai_status_t sai_create_dash_acl_rule(
    _Out_ sai_object_id_t* dash_acl_rule_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

sai_status_t sai_remove_dash_acl_rule(_In_ sai_object_id_t dash_acl_rule_id);

#ifdef __cplusplus
}
#endif
