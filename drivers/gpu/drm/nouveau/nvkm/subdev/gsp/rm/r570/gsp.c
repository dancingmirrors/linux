/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include <rm/rm.h>
#include <rm/rpc.h>

#include <core/option.h>

#include <asm-generic/video.h>

#include "nvrm/fb.h"
#include "nvrm/gsp.h"
#include "nvrm/rpcfn.h"
#include "nvrm/msgfn.h"

#include <core/pci.h>
#include <subdev/pci/priv.h>

static u32
r570_gsp_sr_data_size(struct nvkm_gsp *gsp)
{
	GspFwWprMeta *meta = gsp->wpr_meta.data;

	return (meta->frtsOffset + meta->frtsSize) -
	       (meta->nonWprHeapOffset + meta->nonWprHeapSize);
}

static void
r570_gsp_drop_post_nocat_record(struct nvkm_gsp *gsp)
{
	if (gsp->subdev.debug < NV_DBG_DEBUG) {
		r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD, NULL, NULL);
		r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE, NULL, NULL);
	}
}

static bool
r570_gsp_xlat_mc_engine_idx(u32 mc_engine_idx, enum nvkm_subdev_type *ptype, int *pinst)
{
	switch (mc_engine_idx) {
	case MC_ENGINE_IDX_GSP:
		*ptype = NVKM_SUBDEV_GSP;
		*pinst = 0;
		return true;
	case MC_ENGINE_IDX_DISP:
		*ptype = NVKM_ENGINE_DISP;
		*pinst = 0;
		return true;
	case MC_ENGINE_IDX_DISP_LOW:
		/* GB20x+ report a separate low-latency display vector, used
		 * for head-timing interrupts. Expose it as a second DISP
		 * interrupt instance. r535_disp_oneinit() attaches the
		 * handler to it when the chip's gsp.intr_low_latency is set.
		 */
		*ptype = NVKM_ENGINE_DISP;
		*pinst = 1;
		return true;
	case MC_ENGINE_IDX_CE0 ... MC_ENGINE_IDX_CE19:
		*ptype = NVKM_ENGINE_CE;
		*pinst = mc_engine_idx - MC_ENGINE_IDX_CE0;
		return true;
	case MC_ENGINE_IDX_GR0:
		*ptype = NVKM_ENGINE_GR;
		*pinst = 0;
		return true;
	case MC_ENGINE_IDX_NVDEC0 ... MC_ENGINE_IDX_NVDEC7:
		*ptype = NVKM_ENGINE_NVDEC;
		*pinst = mc_engine_idx - MC_ENGINE_IDX_NVDEC0;
		return true;
	case MC_ENGINE_IDX_NVENC ... MC_ENGINE_IDX_NVENC3:
		*ptype = NVKM_ENGINE_NVENC;
		*pinst = mc_engine_idx - MC_ENGINE_IDX_NVENC;
		return true;
	case MC_ENGINE_IDX_NVJPEG0 ... MC_ENGINE_IDX_NVJPEG7:
		*ptype = NVKM_ENGINE_NVJPG;
		*pinst = mc_engine_idx - MC_ENGINE_IDX_NVJPEG0;
		return true;
	case MC_ENGINE_IDX_OFA0 ... MC_ENGINE_IDX_OFA1:
		*ptype = NVKM_ENGINE_OFA;
		*pinst = mc_engine_idx - MC_ENGINE_IDX_OFA0;
		return true;
	default:
		return false;
	}
}

static const char *
r570_gsp_comptag_policy(u32 policy)
{
	switch (policy) {
	case NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO_POLICY_LEGACY:
		return "legacy";
	case NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO_POLICY_1TO1:
		return "1:1";
	case NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO_POLICY_1TO4:
		return "1:4";
	case NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO_POLICY_RAWMODE:
		return "raw";
	default:
		return "unknown";
	}
}

static u64
r570_gsp_get_compbit_store_info(struct nvkm_gsp *gsp, u32 slices)
{
	NV0080_CTRL_FB_GET_COMPBIT_STORE_INFO_PARAMS *ctrl;
	const char *policy, *aspace;
	u64 coverage;

	ctrl = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.object,
				   NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO,
				   sizeof(*ctrl));
	if (IS_ERR(ctrl)) {
		nvkm_warn(&gsp->subdev, "failed to read compbit store info\n");
		return 0;
	}

	policy = r570_gsp_comptag_policy(ctrl->comptaglineAllocationPolicy);
	coverage = (u64)ctrl->cbcCoveragePerSlice * slices;

	switch (ctrl->AddressSpace) {
	case NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO_ADDRESS_SPACE_FBMEM:
		aspace = "VRAM";
		break;
	case NV0080_CTRL_CMD_FB_GET_COMPBIT_STORE_INFO_ADDRESS_SPACE_SYSMEM:
		aspace = "system memory";
		break;
	default:
		aspace = "unknown memory";
		break;
	}

	if (ctrl->Size)
		nvkm_debug(&gsp->subdev, "cbc: %s comptags, %llu KiB backing store at 0x%llx in %s\n",
			   policy, ctrl->Size >> 10, ctrl->Address, aspace);
	else
		nvkm_debug(&gsp->subdev, "cbc: %s comptags, no backing store\n", policy);

	if (ctrl->MaxCompbitLine)
		nvkm_debug(&gsp->subdev, "cbc: %u comptag lines, for %llu MiB\n",
			   ctrl->MaxCompbitLine,
			   ((u64)ctrl->MaxCompbitLine << gsp->fb.comp.page_shift) >> 20);

	nvkm_debug(&gsp->subdev,
		   "cbc: compbit cache covers %llu MiB (%u KiB x %u slices), %u tags per %u B line\n",
		   coverage >> 20, ctrl->cbcCoveragePerSlice >> 10, slices,
		   ctrl->comptagsPerCacheLine, ctrl->cacheLineSize);

	gsp->fb.comp.cbc_size = ctrl->Size;

	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.object, ctrl);
	return coverage;
}

static void
r570_gsp_get_static_info_memsys(struct nvkm_gsp *gsp)
{
	NV2080_CTRL_INTERNAL_MEMSYS_GET_STATIC_CONFIG_PARAMS *ctrl;
	struct nvkm_device *device = gsp->subdev.device;
	const bool gb20x = device->chipset >= 0x1b0 && device->chipset < 0x1c0;
	u64 dflt = 0, coverage = 0;
	bool fw_comp;
	u32 slices;
	long opt;

	gsp->fb.comp.disabled = true;

	ctrl = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.subdevice,
				   NV2080_CTRL_CMD_INTERNAL_MEMSYS_GET_STATIC_CONFIG,
				   sizeof(*ctrl));
	if (IS_ERR(ctrl)) {
		nvkm_warn(&gsp->subdev, "failed to read memsys config, compression disabled\n");
		return;
	}

	if (!ctrl->bDisableCompbitBacking &&
	    (ctrl->bOneToOneComptagLineAllocation || ctrl->bUseRawModeComptaglineAllocation)) {
		gsp->fb.comp.disabled = false;
		gsp->fb.comp.plc_disabled = ctrl->bDisablePostL2Compression;
		gsp->fb.comp.page_shift = ctrl->comprPageShift;
	}

	fw_comp = !gsp->fb.comp.disabled;
	if (fw_comp && !nvkm_boolopt(device->cfgopt, "NvComp", true)) {
		nvkm_info(&gsp->subdev, "comp: VRAM compression disabled by NvComp=0\n");
		gsp->fb.comp.disabled = true;
	}

	if (!gsp->fb.comp.disabled && !gsp->fb.comp.plc_disabled &&
	    (device->card_type == GH100 || device->card_type >= GB10x) &&
	    !nvkm_boolopt(device->cfgopt, "NvCompPlc", true)) {
		nvkm_info(&gsp->subdev, "comp: post-L2 compression disabled by NvCompPlc=0\n");
		gsp->fb.comp.plc_disabled = true;
	}

	if (!fw_comp)
		nvkm_debug(&gsp->subdev, "comp: unavailable (backing:%d 1:1:%d 1:4:%d raw:%d)\n",
			   !ctrl->bDisableCompbitBacking, ctrl->bOneToOneComptagLineAllocation,
			   ctrl->bUseOneToFourComptagLineAllocation,
			   ctrl->bUseRawModeComptaglineAllocation);
	else if (gsp->fb.comp.disabled)
		nvkm_debug(&gsp->subdev, "comp: off by NvComp=0\n");
	else
		nvkm_debug(&gsp->subdev, "comp: %s comptags, %u KiB pages, post-L2 compression %s\n",
			   ctrl->bUseRawModeComptaglineAllocation ? "raw" : "1:1",
			   ctrl->comprPageSize >> 10,
			   gsp->fb.comp.plc_disabled ? "off" : "on");

	slices = ctrl->ltcCount * ctrl->ltsPerLtcCount;

	nvkm_debug(&gsp->subdev, "comp: %llu KiB of L2 in %u slices (%u x %u)\n",
		   ctrl->l2CacheSize >> 10, slices, ctrl->ltcCount, ctrl->ltsPerLtcCount);

	if (gb20x && slices && ctrl->comprPageSize)
		dflt = 256ULL * slices * ctrl->comprPageSize;

	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);

	if (fw_comp)
		coverage = r570_gsp_get_compbit_store_info(gsp, slices);

	if (gsp->fb.comp.disabled)
		return;

	if (dflt && coverage && coverage < dflt)
		dflt = coverage;

	opt = nvkm_longopt(device->cfgopt, "NvCompLimitMiB", -1);
	if (opt >= 0)
		dflt = min_t(u64, opt, 1024 * 1024) << 20;

	gsp->fb.comp.limit = dflt;

	if (!gsp->fb.comp.limit)
		nvkm_debug(&gsp->subdev, "comp: no budget%s\n",
			   opt == 0 ? ", NvCompLimitMiB=0" : "");
	else
		nvkm_debug(&gsp->subdev, "comp: budget of %llu MiB%s\n",
			   gsp->fb.comp.limit >> 20,
			   opt > 0 ? ", NvCompLimitMiB" : ", the GB20x default");
}

static int
r570_gsp_get_static_info(struct nvkm_gsp *gsp)
{
	GspStaticConfigInfo *rpc;
	u32 gpc_mask;
	u32 tpc_mask;
	int ret;

	rpc = nvkm_gsp_rpc_rd(gsp, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, sizeof(*rpc));
	if (IS_ERR(rpc))
		return PTR_ERR(rpc);

	gsp->internal.client.object.client = &gsp->internal.client;
	gsp->internal.client.object.parent = NULL;
	gsp->internal.client.object.handle = rpc->hInternalClient;
	gsp->internal.client.gsp = gsp;
	INIT_LIST_HEAD(&gsp->internal.client.events);

	gsp->internal.device.object.client = &gsp->internal.client;
	gsp->internal.device.object.parent = &gsp->internal.client.object;
	gsp->internal.device.object.handle = rpc->hInternalDevice;

	gsp->internal.device.subdevice.client = &gsp->internal.client;
	gsp->internal.device.subdevice.parent = &gsp->internal.device.object;
	gsp->internal.device.subdevice.handle = rpc->hInternalSubdevice;

	gsp->bar.rm_bar1_pdb = rpc->bar1PdeBase;
	gsp->bar.rm_bar2_pdb = rpc->bar2PdeBase;

	r535_gsp_get_static_info_fb(gsp, &rpc->fbRegionInfoParams);

	if (gsp->rm->wpr->offset_set_by_acr) {
		GspFwWprMeta *meta = gsp->wpr_meta.data;

		meta->nonWprHeapOffset = rpc->fwWprLayoutOffset.nonWprHeapOffset;
		meta->frtsOffset = rpc->fwWprLayoutOffset.frtsOffset;
	}

	nvkm_gsp_rpc_done(gsp, rpc);

	ret = r570_gr_gpc_mask(gsp, &gpc_mask);
	if (ret)
		return ret;

	for (int gpc = 0; gpc < 32; gpc++) {
		if (gpc_mask & BIT(gpc)) {
			ret = r570_gr_tpc_mask(gsp, gpc, &tpc_mask);
			if (ret)
				return ret;

			gsp->gr.tpcs += hweight32(tpc_mask);
			gsp->gr.gpcs++;
		}
	}

	r570_gsp_get_static_info_memsys(gsp);
	return 0;
}

static void
r570_gsp_acpi_info(struct nvkm_gsp *gsp, ACPI_METHOD_DATA *acpi)
{
#if defined(CONFIG_ACPI) && defined(CONFIG_X86)
	acpi_handle handle = ACPI_HANDLE(gsp->subdev.device->dev);

	if (!handle)
		return;

	acpi->bValid = 1;

	r535_gsp_acpi_dod(handle, &acpi->dodMethodData);
	r535_gsp_acpi_jt(handle, &acpi->jtMethodData);
	r535_gsp_acpi_caps(handle, &acpi->capsMethodData);
#endif
}

static int
r570_gsp_set_system_info(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	struct pci_dev *pdev = container_of(device, struct nvkm_device_pci, device)->pdev;
	GspSystemInfo *info;

	if (WARN_ON(device->type == NVKM_DEVICE_TEGRA))
		return -ENOSYS;

	info = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, sizeof(*info));
	if (IS_ERR(info))
		return PTR_ERR(info);

	info->gpuPhysAddr = device->func->resource_addr(device, NVKM_BAR0_PRI);
	info->gpuPhysFbAddr = device->func->resource_addr(device, NVKM_BAR1_FB);
	info->gpuPhysInstAddr = device->func->resource_addr(device, NVKM_BAR2_INST);
	info->nvDomainBusDeviceFunc = pci_dev_id(pdev);
	info->maxUserVa = TASK_SIZE;
	info->pciConfigMirrorBase = device->pci->func->cfg.addr;
	info->pciConfigMirrorSize = device->pci->func->cfg.size;
	info->PCIDeviceID = (pdev->device << 16) | pdev->vendor;
	info->PCISubDeviceID = (pdev->subsystem_device << 16) | pdev->subsystem_vendor;
	info->PCIRevisionID = pdev->revision;
	r570_gsp_acpi_info(gsp, &info->acpiMethodData);
	info->bIsPrimary = video_is_primary_device(device->dev);

	gsp->fb.preserve_vidmem = nvkm_boolopt(device->cfgopt, "NvPreserveVidmem", false);
	info->bPreserveVideoMemoryAllocations = gsp->fb.preserve_vidmem;

	return nvkm_gsp_rpc_wr(gsp, info, NVKM_GSP_RPC_REPLY_NOSEQ);
}

static void
r570_gsp_set_rmargs(struct nvkm_gsp *gsp, bool resume)
{
	GSP_ARGUMENTS_CACHED *args;

	args = gsp->rmargs.data;
	args->messageQueueInitArguments.sharedMemPhysAddr = gsp->shm.mem.addr;
	args->messageQueueInitArguments.pageTableEntryCount = gsp->shm.ptes.nr;
	args->messageQueueInitArguments.cmdQueueOffset =
		(u8 *)gsp->shm.cmdq.ptr - (u8 *)gsp->shm.mem.data;
	args->messageQueueInitArguments.statQueueOffset =
		(u8 *)gsp->shm.msgq.ptr - (u8 *)gsp->shm.mem.data;

	if (!resume) {
		args->srInitArguments.oldLevel = 0;
		args->srInitArguments.flags = 0;
		args->srInitArguments.bInPMTransition = 0;
	} else {
		args->srInitArguments.oldLevel = NV2080_CTRL_GPU_SET_POWER_STATE_GPU_LEVEL_3;
		args->srInitArguments.flags =
			GPU_STATE_FLAGS_PRESERVING | GPU_STATE_FLAGS_PM_TRANSITION;
		args->srInitArguments.bInPMTransition = 1;
	}

	args->bDmemStack = 1;
}

const struct nvkm_rm_api_gsp
r570_gsp = {
	.set_rmargs = r570_gsp_set_rmargs,
	.set_system_info = r570_gsp_set_system_info,
	.get_static_info = r570_gsp_get_static_info,
	.xlat_mc_engine_idx = r570_gsp_xlat_mc_engine_idx,
	.drop_post_nocat_record = r570_gsp_drop_post_nocat_record,
	.sr_data_size = r570_gsp_sr_data_size,
};
