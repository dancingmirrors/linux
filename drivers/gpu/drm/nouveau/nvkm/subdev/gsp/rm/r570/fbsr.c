/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include <subdev/instmem/priv.h>
#include <subdev/bar.h>
#include <subdev/gsp.h>
#include <subdev/mmu/vmm.h>

#include "nvrm/fbsr.h"
#include "nvrm/fifo.h"

static int
r570_fbsr_suspend_channels(struct nvkm_gsp *gsp, bool suspend)
{
	NV2080_CTRL_CMD_INTERNAL_FIFO_TOGGLE_ACTIVE_CHANNEL_SCHEDULING_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    NV2080_CTRL_CMD_INTERNAL_FIFO_TOGGLE_ACTIVE_CHANNEL_SCHEDULING,
				    sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->bDisableActiveChannels = suspend;

	return nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, ctrl);
}

static void
r570_fbsr_resume(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	struct nvkm_instmem *imem = device->imem;
	struct nvkm_instobj *iobj;
	struct nvkm_vmm *vmm;

	/* Restore BAR2 page tables via BAR0 window, and re-enable BAR2. */
	list_for_each_entry(iobj, &imem->boot, head) {
		if (iobj->suspend)
			nvkm_instobj_load(iobj);
	}

	device->bar->bar2 = true;

	vmm = nvkm_bar_bar2_vmm(device);
	vmm->func->flush(vmm, 0);

	/* Restore remaining BAR2 allocations (including BAR1 page tables) via BAR2. */
	list_for_each_entry(iobj, &imem->list, head) {
		if (iobj->suspend)
			nvkm_instobj_load(iobj);
	}

	vmm = nvkm_bar_bar1_vmm(device);
	vmm->func->flush(vmm, 0);

	/* Resume channel scheduling. */
	r570_fbsr_suspend_channels(device->gsp, false);

	/* Finish cleaning up. */
	r535_fbsr_resume(gsp);
}

static int
r570_fbsr_init(struct nvkm_gsp *gsp, struct sg_table *sgt, u64 size)
{
	NV2080_CTRL_INTERNAL_FBSR_INIT_PARAMS *ctrl;
	struct nvkm_gsp_object memlist;
	int ret;

	ret = r535_fbsr_memlist(&gsp->internal.device, 0xcaf00003, NVKM_MEM_TARGET_HOST,
				0, size, sgt, &memlist);
	if (ret)
		return ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    NV2080_CTRL_CMD_INTERNAL_FBSR_INIT, sizeof(*ctrl));
	if (IS_ERR(ctrl)) {
		ret = PTR_ERR(ctrl);
		goto done;
	}

	ctrl->hClient = gsp->internal.client.object.handle;
	ctrl->hSysMem = memlist.handle;
	ctrl->sysmemAddrOfSuspendResumeData = gsp->sr.meta.addr;
	ctrl->bEnteringGcoffState = gsp->sr.gcoff;

	ret = nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, ctrl);
done:
	nvkm_gsp_rm_free(&memlist);
	return ret;
}

static void
r570_fbsr_unwind(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;
	struct nvkm_instmem *imem = device->imem;
	struct nvkm_instobj *iobj;

	list_for_each_entry(iobj, &imem->list, head) {
		kvfree(iobj->suspend);
		iobj->suspend = NULL;
	}

	list_for_each_entry(iobj, &imem->boot, head) {
		kvfree(iobj->suspend);
		iobj->suspend = NULL;
	}

	device->bar->bar2 = true;

	r570_fbsr_suspend_channels(gsp, false);
}

static u64
r570_fbsr_size(struct nvkm_gsp *gsp, bool cbc)
{
	u64 size;

	size  = gsp->fb.heap.size;
	size += gsp->fb.rsvd_size;
	size += gsp->fb.bios.vga_workspace.size;
	if (cbc)
		size += gsp->fb.comp.cbc_size;

	return size;
}

static u64
r570_fbsr_sysmem_size(struct nvkm_gsp *gsp)
{
	struct nvkm_device *device = gsp->subdev.device;

	return r570_fbsr_size(gsp, true) + nvkm_instmem_suspend_size(device->imem);
}

static int
r570_fbsr_suspend(struct nvkm_gsp *gsp)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct nvkm_device *device = subdev->device;
	struct nvkm_instmem *imem = device->imem;
	struct nvkm_instobj *iobj;
	u64 size;
	int ret;

	/* Stop channel scheduling. */
	r570_fbsr_suspend_channels(gsp, true);

	/* Save BAR2 allocations to system memory. */
	list_for_each_entry(iobj, &imem->list, head) {
		if (iobj->preserve) {
			ret = nvkm_instobj_save(iobj);
			if (ret)
				goto fail;
		}
	}

	list_for_each_entry(iobj, &imem->boot, head) {
		ret = nvkm_instobj_save(iobj);
		if (ret)
			goto fail;
	}

	/* Disable BAR2 access. */
	device->bar->bar2 = false;

	/* Allocate system memory to hold RM's VRAM allocations across suspend. */
	size = r570_fbsr_size(gsp, gsp->fb.preserve_vidmem || gsp->sr.gcoff);
	nvkm_debug(subdev, "fbsr: size: 0x%llx bytes (gcoff:%d)\n", size, gsp->sr.gcoff);

	ret = nvkm_gsp_sg(device, size, &gsp->sr.fbsr);
	if (ret)
		goto fail;

	/* Initialize FBSR on RM. */
	ret = r570_fbsr_init(gsp, &gsp->sr.fbsr, size);
	if (ret) {
		nvkm_gsp_sg_free(device, &gsp->sr.fbsr);
		goto fail;
	}

	return 0;

fail:
	r570_fbsr_unwind(gsp);
	return ret;
}

const struct nvkm_rm_api_fbsr
r570_fbsr = {
	.suspend = r570_fbsr_suspend,
	.resume = r570_fbsr_resume,
	.sysmem_size = r570_fbsr_sysmem_size,
};
