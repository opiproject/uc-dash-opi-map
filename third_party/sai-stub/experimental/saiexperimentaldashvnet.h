/**
 * @file saiexperimentaldashvnet.h
 * @brief Minimal DASH VNET API (subset of OCP SAI experimental header).
 */
#ifndef SAIEXPERIMENTALDASHVNET_H_
#define SAIEXPERIMENTALDASHVNET_H_

#include <saistatus.h>
#include <saitypesextensions.h>

typedef enum _sai_vnet_attr_t {
  SAI_VNET_ATTR_START,
  SAI_VNET_ATTR_VNI = SAI_VNET_ATTR_START,
  SAI_VNET_ATTR_END,
  SAI_VNET_ATTR_CUSTOM_RANGE_START = 0x10000000,
  SAI_VNET_ATTR_CUSTOM_RANGE_END
} sai_vnet_attr_t;

typedef sai_status_t (*sai_create_vnet_fn)(
    _Out_ sai_object_id_t* vnet_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

typedef sai_status_t (*sai_remove_vnet_fn)(_In_ sai_object_id_t vnet_id);

typedef struct _sai_dash_vnet_api_t {
  sai_create_vnet_fn create_vnet;
  sai_remove_vnet_fn remove_vnet;
} sai_dash_vnet_api_t;

#endif
