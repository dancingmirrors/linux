/* SPDX-License-Identifier: MIT */

/* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved. */

#ifndef __NVRM_FB_H__
#define __NVRM_FB_H__
#include <nvrm/nvtypes.h>

/* Excerpt of RM headers from https://github.com/NVIDIA/open-gpu-kernel-modules/tree/570.144 */

typedef struct NV2080_CTRL_INTERNAL_MEMSYS_GET_STATIC_CONFIG_PARAMS {
	NvBool bOneToOneComptagLineAllocation;
	NvBool bUseOneToFourComptagLineAllocation;
	NvBool bUseRawModeComptaglineAllocation;
	NvBool bDisableCompbitBacking;
	NvBool bDisablePostL2Compression;
	NvBool bEnabledEccFBPA;
	NvBool bL2PreFill;
	NV_DECLARE_ALIGNED(NvU64 l2CacheSize, 8);
	NvBool bFbpaPresent;
	NvU32  comprPageSize;
	NvU32  comprPageShift;
	NvU32  ramType;
	NvU32  ltcCount;
	NvU32  ltsPerLtcCount;
} NV2080_CTRL_INTERNAL_MEMSYS_GET_STATIC_CONFIG_PARAMS;

#define NV2080_CTRL_CMD_INTERNAL_MEMSYS_GET_STATIC_CONFIG (0x20800a1c)

#endif
