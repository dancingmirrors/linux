/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#ifndef __gh100_dev_bus_h__
#define __gh100_dev_bus_h__

#define NV_PBUS_SW_SCRATCH(i)                                    (0x00001400+(i)*4)
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR                         NV_PBUS_SW_SCRATCH(0x37)
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_VARIANT                 31:28
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_VARIANT_SK              0x00000000
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_VARIANT_GENERIC         0x00000001
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_PARTITION               27:24
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_PAYLOAD                 23:0
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_SK_ERROR                15:8
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_SK_PHASE                7:0
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_GENERIC_ADDITIONAL_INFO 23:16
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_GENERIC_ERROR_CODE      15:0
#define NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR_PARTITION_BIAS          0x00000001

#endif // __gh100_dev_bus_h__
