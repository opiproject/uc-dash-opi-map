/**
 * @file saiexperimentaldashacl.h
 * @brief Minimal DASH ACL API (subset of OCP SAI experimental).
 */
#ifndef SAIEXPERIMENTALDASHACL_H_
#define SAIEXPERIMENTALDASHACL_H_

#include <saistatus.h>
#include <saitypes.h>
#include <saitypesextensions.h>

typedef enum _sai_dash_acl_rule_action_t {
  SAI_DASH_ACL_RULE_ACTION_PERMIT,
  SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE,
  SAI_DASH_ACL_RULE_ACTION_DENY,
  SAI_DASH_ACL_RULE_ACTION_DENY_AND_CONTINUE
} sai_dash_acl_rule_action_t;

typedef enum _sai_dash_acl_group_attr_t {
  SAI_DASH_ACL_GROUP_ATTR_START,
  SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY = SAI_DASH_ACL_GROUP_ATTR_START,
  SAI_DASH_ACL_GROUP_ATTR_END,
  SAI_DASH_ACL_GROUP_ATTR_CUSTOM_RANGE_START = 0x10000000,
  SAI_DASH_ACL_GROUP_ATTR_CUSTOM_RANGE_END
} sai_dash_acl_group_attr_t;

typedef enum _sai_dash_acl_rule_attr_t {
  SAI_DASH_ACL_RULE_ATTR_START,
  SAI_DASH_ACL_RULE_ATTR_ACTION = SAI_DASH_ACL_RULE_ATTR_START,
  SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID,
  SAI_DASH_ACL_RULE_ATTR_DIP,
  SAI_DASH_ACL_RULE_ATTR_SIP,
  SAI_DASH_ACL_RULE_ATTR_PROTOCOL,
  SAI_DASH_ACL_RULE_ATTR_SRC_PORT,
  SAI_DASH_ACL_RULE_ATTR_DST_PORT,
  SAI_DASH_ACL_RULE_ATTR_COUNTER_ID,
  SAI_DASH_ACL_RULE_ATTR_PRIORITY,
  SAI_DASH_ACL_RULE_ATTR_IP_ADDR_FAMILY,
  SAI_DASH_ACL_RULE_ATTR_END,
  SAI_DASH_ACL_RULE_ATTR_CUSTOM_RANGE_START = 0x10000000,
  SAI_DASH_ACL_RULE_ATTR_CUSTOM_RANGE_END
} sai_dash_acl_rule_attr_t;

typedef sai_status_t (*sai_create_dash_acl_group_fn)(
    _Out_ sai_object_id_t* dash_acl_group_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

typedef sai_status_t (*sai_remove_dash_acl_group_fn)(
    _In_ sai_object_id_t dash_acl_group_id);

typedef sai_status_t (*sai_create_dash_acl_rule_fn)(
    _Out_ sai_object_id_t* dash_acl_rule_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

typedef sai_status_t (*sai_remove_dash_acl_rule_fn)(
    _In_ sai_object_id_t dash_acl_rule_id);

typedef struct _sai_dash_acl_api_t {
  sai_create_dash_acl_group_fn create_dash_acl_group;
  sai_remove_dash_acl_group_fn remove_dash_acl_group;
  sai_create_dash_acl_rule_fn create_dash_acl_rule;
  sai_remove_dash_acl_rule_fn remove_dash_acl_rule;
} sai_dash_acl_api_t;

#endif
