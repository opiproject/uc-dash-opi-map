#ifndef SAIEXPERIMENTALDASHENI_H_
#define SAIEXPERIMENTALDASHENI_H_

#include <saistatus.h>
#include <saitypesextensions.h>

typedef enum _sai_eni_attr_t {
  SAI_ENI_ATTR_START,
  SAI_ENI_ATTR_CPS = SAI_ENI_ATTR_START,
  SAI_ENI_ATTR_PPS,
  SAI_ENI_ATTR_FLOWS,
  SAI_ENI_ATTR_ADMIN_STATE,
  SAI_ENI_ATTR_HA_SCOPE_ID,
  SAI_ENI_ATTR_VM_UNDERLAY_DIP,
  SAI_ENI_ATTR_VM_VNI,
  SAI_ENI_ATTR_VNET_ID,
  SAI_ENI_ATTR_DASH_ENI_MODE = SAI_ENI_ATTR_VNET_ID + 20,
  SAI_ENI_ATTR_END,
  SAI_ENI_ATTR_CUSTOM_RANGE_START = 0x10000000,
  SAI_ENI_ATTR_CUSTOM_RANGE_END
} sai_eni_attr_t;

typedef sai_status_t (*sai_create_eni_fn)(
    _Out_ sai_object_id_t* eni_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

typedef sai_status_t (*sai_remove_eni_fn)(_In_ sai_object_id_t eni_id);

/**
 * Counter IDs match OCP saiexperimentaldasheni.h (explicit values).
 */
typedef enum _sai_eni_stat_t {
  SAI_ENI_STAT_RX_BYTES = 0,
  SAI_ENI_STAT_RX_PACKETS = 1,
  SAI_ENI_STAT_TX_BYTES = 2,
  SAI_ENI_STAT_TX_PACKETS = 3,
  SAI_ENI_STAT_FORWARDING_DROP_PACKETS = 94,
  SAI_ENI_STAT_POLICY_DROP_PACKETS = 95,
  SAI_ENI_STAT_OTHER_DROP_PACKETS = 102,
  SAI_ENI_STAT_TOTAL_DROP_PACKETS = 103
} sai_eni_stat_t;

typedef sai_status_t (*sai_get_eni_stats_fn)(
    _In_ sai_object_id_t eni_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _Out_ uint64_t* counters);

typedef struct _sai_dash_eni_api_t {
  sai_create_eni_fn create_eni;
  sai_remove_eni_fn remove_eni;
  sai_get_eni_stats_fn get_eni_stats;
} sai_dash_eni_api_t;

#endif
