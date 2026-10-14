#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sai.h>

/**
 * @brief Create ENI — SAI vendor entry that dashorch calls via
 *        sai_dash_eni_api_t.create_eni.
 *
 * Signature matches sai_create_eni_fn in saiexperimentaldasheni.h.
 */
sai_status_t sai_create_eni(
    _Out_ sai_object_id_t* eni_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t* attr_list);

/**
 * @brief Remove ENI — DeleteVnic then DeleteInterface.
 * Signature matches sai_remove_eni_fn in saiexperimentaldasheni.h.
 * Unknown oid returns SAI_STATUS_ITEM_NOT_FOUND.
 */
sai_status_t sai_remove_eni(_In_ sai_object_id_t eni_id);

/**
 * @brief Get ENI statistics counters.
 * Signature matches sai_get_eni_stats_fn in saiexperimentaldasheni.h.
 * Looks up the ENI's Vnic name, calls TelemetryService.GetVnicMetrics on
 * the OPI inventory/telemetry endpoint, and fills counters[] for FlexCounter.
 */
sai_status_t sai_get_eni_stats(
    _In_ sai_object_id_t eni_id,
    _In_ uint32_t number_of_counters,
    _In_ const sai_stat_id_t* counter_ids,
    _Out_ uint64_t* counters);

#ifdef __cplusplus
}
#endif
