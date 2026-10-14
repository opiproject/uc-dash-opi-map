#ifndef SAI_H_
#define SAI_H_

#include <saistatus.h>
#include <saitypes.h>

sai_status_t sai_api_initialize(
    _In_ uint64_t flags,
    _In_ const sai_service_method_table_t* services);

sai_status_t sai_api_query(_In_ sai_api_t api, _Out_ void** api_method_table);

sai_status_t sai_api_uninitialize(void);

sai_status_t sai_log_set(_In_ sai_api_t api, _In_ sai_log_level_t log_level);

#endif
