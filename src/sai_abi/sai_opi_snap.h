#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <sai.h>

/**
 * Create an emulated NVMe controller on SNAP (FrontendNvmeService).
 * parent example: "nvmeSubsystems/subsystem2"
 * nvme_controller_id example: "controller1"
 * NULL/empty uses SAI_OPI_NVME_PARENT / SAI_OPI_NVME_CONTROLLER_ID, then
 * the NVIDIA SNAP README defaults.
 */
sai_status_t sai_opi_create_nvme_controller(
    _In_ const char* parent,
    _In_ const char* nvme_controller_id);

/**
 * Enable IPsec offload on SNAP (IPsecService). Constructs IPsecInitiateRequest
 * then IPsecRekeyRequest (informally CreateIPsecRekeyRequest) for ike_name.
 * NULL/empty uses SAI_OPI_IPSEC_IKE, then "ike".
 */
sai_status_t sai_opi_enable_ipsec(_In_ const char* ike_name);

#ifdef __cplusplus
}
#endif
