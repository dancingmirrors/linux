/* SPDX-License-Identifier: MIT */
#include "priv.h"

#include <linux/delay.h>

#include <nvhw/drf.h>
#include <nvhw/ref/gh100/dev_fsp_addendum.h>
#include <nvhw/ref/gh100/dev_fsp_pri.h>
#include <nvhw/ref/gh100/dev_gc6_island.h>

static u64
gh100_timer_sec_read(struct nvkm_device *device)
{
	u32 hi, lo;

	do {
		hi = nvkm_rd32(device, NV_PGC6_SCI_SEC_TIMER_TIME_1);
		lo = nvkm_rd32(device, NV_PGC6_SCI_SEC_TIMER_TIME_0);
	} while (hi != nvkm_rd32(device, NV_PGC6_SCI_SEC_TIMER_TIME_1));

	return ((u64)hi << 32) | lo;
}

static bool
gh100_timer_offset_writable(struct nvkm_device *device)
{
	u32 version;

	if (device->chipset != 0x180)
		return true;

	version = NVVAL_GET(nvkm_rd32(device, NV_GFW_FSP_UCODE_VERSION),
			    NV_GFW, FSP_UCODE_VERSION, FULL);
	return version >= 0x44c;
}

static void
gh100_timer_time(struct nvkm_timer *tmr, u64 time)
{
	struct nvkm_subdev *subdev = &tmr->subdev;
	struct nvkm_device *device = subdev->device;
	u64 sec = gh100_timer_sec_read(device);
	u64 offset;
	int i;

	if (sec >= time) {
		nvkm_warn(subdev, "sec timer %016llx is ahead of %016llx, not set\n",
			  sec, time);
		return;
	}

	if (!gh100_timer_offset_writable(device)) {
		nvkm_debug(subdev, "FSP too old to program the sys timer offset\n");
		return;
	}

	offset = time - sec;

	nvkm_debug(subdev, "sec timer       : %016llx\n", sec);
	nvkm_debug(subdev, "sys timer offset: %016llx\n", offset);

	nvkm_wr32(device, NV_PGC6_SCI_SYS_TIMER_OFFSET_1, upper_32_bits(offset));
	nvkm_wr32(device, NV_PGC6_SCI_SYS_TIMER_OFFSET_0, lower_32_bits(offset) |
		  NVDEF(NV_PGC6, SCI_SYS_TIMER_OFFSET_0, UPDATE, TRIGGER));

	for (i = 0; i < 1000; i++) {
		u32 ctrl = nvkm_rd32(device, NV_PGC6_SCI_SYS_TIMER_OFFSET_0);

		if (NVVAL_GET(ctrl, NV_PGC6, SCI_SYS_TIMER_OFFSET_0, UPDATE) ==
		    NV_PGC6_SCI_SYS_TIMER_OFFSET_0_UPDATE_DONE)
			break;

		udelay(1);
	}

	if (i == 1000)
		nvkm_warn(subdev, "sys timer offset update didn't complete\n");

	nvkm_debug(subdev, "ptimer          : %016llx\n", nvkm_timer_read(tmr));
}

static const struct nvkm_timer_func
gh100_timer = {
	.intr = nv04_timer_intr,
	.read = nv04_timer_read,
	.time = gh100_timer_time,
	.alarm_init = nv04_timer_alarm_init,
	.alarm_fini = nv04_timer_alarm_fini,
};

int
gh100_timer_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
		struct nvkm_timer **ptmr)
{
	return nvkm_timer_new_(&gh100_timer, device, type, inst, ptmr);
}
