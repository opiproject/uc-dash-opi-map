/**
 * @file saiexperimentaldashoutboundrouting.h
 * @brief Minimal DASH outbound routing API (subset of OCP SAI experimental).
 */
#ifndef SAIEXPERIMENTALDASHOUTBOUNDROUTING_H_
#define SAIEXPERIMENTALDASHOUTBOUNDROUTING_H_

#include <saistatus.h>
#include <saitypes.h>
#include <saitypesextensions.h>

typedef enum _sai_outbound_routing_entry_action_t {
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_DIRECT,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_SERVICE_TUNNEL,
  SAI_OUTBOUND_ROUTING_ENTRY_ACTION_DROP
} sai_outbound_routing_entry_action_t;

typedef struct _sai_outbound_routing_entry_t {
  sai_object_id_t switch_id;
  sai_ip_prefix_t destination;
  sai_object_id_t outbound_routing_group_id;
} sai_outbound_routing_entry_t;

typedef enum _sai_outbound_routing_entry_attr_t {
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_START,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION =
      SAI_OUTBOUND_ROUTING_ENTRY_ATTR_START,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DASH_TUNNEL_ID,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_OVERLAY_IP =
      SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DASH_TUNNEL_ID + 4,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_TUNNEL_KEY =
      SAI_OUTBOUND_ROUTING_ENTRY_ATTR_OVERLAY_IP + 6,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_END,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_CUSTOM_RANGE_START = 0x10000000,
  SAI_OUTBOUND_ROUTING_ENTRY_ATTR_CUSTOM_RANGE_END
} sai_outbound_routing_entry_attr_t;

typedef enum _sai_outbound_routing_group_attr_t {
  SAI_OUTBOUND_ROUTING_GROUP_ATTR_START,
  SAI_OUTBOUND_ROUTING_GROUP_ATTR_DISABLED =
      SAI_OUTBOUND_ROUTING_GROUP_ATTR_START,
  SAI_OUTBOUND_ROUTING_GROUP_ATTR_END,
  SAI_OUTBOUND_ROUTING_GROUP_ATTR_CUSTOM_RANGE_START = 0x10000000,
  SAI_OUTBOUND_ROUTING_GROUP_ATTR_CUSTOM_RANGE_END
} sai_outbound_routing_group_attr_t;

typedef sai_status_t (*sai_create_outbound_routing_group_fn)(
    _Out_ sai_object_id_t* outbound_routing_group_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

typedef sai_status_t (*sai_remove_outbound_routing_group_fn)(
    _In_ sai_object_id_t outbound_routing_group_id);

typedef sai_status_t (*sai_create_outbound_routing_entry_fn)(
    _In_ const sai_outbound_routing_entry_t* outbound_routing_entry,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

typedef sai_status_t (*sai_remove_outbound_routing_entry_fn)(
    _In_ const sai_outbound_routing_entry_t* outbound_routing_entry);

typedef struct _sai_dash_outbound_routing_api_t {
  sai_create_outbound_routing_entry_fn create_outbound_routing_entry;
  sai_remove_outbound_routing_entry_fn remove_outbound_routing_entry;
  sai_create_outbound_routing_group_fn create_outbound_routing_group;
  sai_remove_outbound_routing_group_fn remove_outbound_routing_group;
} sai_dash_outbound_routing_api_t;

#endif
