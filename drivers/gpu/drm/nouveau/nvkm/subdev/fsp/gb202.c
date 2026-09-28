/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include "priv.h"

#include <linux/ktime.h>
#include <linux/pci.h>

#include <subdev/pci.h>

#include <nvhw/drf.h>
#include <nvhw/ref/gb202/dev_therm.h>
#include <nvhw/ref/gb202/dev_xtl_ep_pcfg_gpu.h>

static void
gb202_fsp_wait_bar_firewall(struct nvkm_fsp *fsp)
{
	struct nvkm_subdev *subdev = &fsp->subdev;
	struct nvkm_device *device = subdev->device;
	struct pci_dev *pdev = device->pci ? device->pci->pdev : NULL;
	const ktime_t start = ktime_get();
	bool absent = false;
	int polls = 0;
	u32 val;

	if (!pdev)
		return;

	do {
		if (pci_read_config_dword(pdev, NV_EP_PCFG_GPU_VSEC_DEBUG_SEC_2, &val))
			return;

		absent = val == 0xffffffff;
		if (!absent && NVDEF_TEST(val, NV_EP_PCFG_GPU, VSEC_DEBUG_SEC_2,
					  BAR_FIREWALL_ENGAGE, ==, INIT)) {
			if (polls)
				nvkm_debug(subdev, "BAR firewall released after %lldus\n",
					   ktime_us_delta(ktime_get(), start));
			return;
		}

		usleep_range(100, 200);
		polls++;
	} while (ktime_ms_delta(ktime_get(), start) < 1000);

	if (absent)
		nvkm_warn(subdev, "config space still not answering after ~1s, continuing\n");
	else
		nvkm_warn(subdev, "BAR firewall still engaged after ~1s (0x%08x), continuing\n",
			  val);
}

static int
gb202_fsp_wait_secure_boot(struct nvkm_fsp *fsp)
{
	struct nvkm_device *device = fsp->subdev.device;
	const unsigned int timeout_ms = 5000;
	unsigned int time = timeout_ms;
	u32 status;

	do {
		status = NVKM_RD32(device, NV_THERM, I2CS_SCRATCH, FSP_BOOT_COMPLETE_STATUS);
		if (status == NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE_STATUS_SUCCESS)
			return 0;

		usleep_range(1000, 2000);
	} while (time--);

	gh100_fsp_boot_timeout(fsp, status, timeout_ms);
	return -ETIMEDOUT;
}

static const struct nvkm_fsp_func
gb202_fsp = {
	.wait_bar_firewall = gb202_fsp_wait_bar_firewall,
	.wait_secure_boot = gb202_fsp_wait_secure_boot,
	.cot = {
		.version = 2,
		.size_hash = 48,
		.size_pkey = 97,
		.size_sig = 96,
		.boot_gsp_fmc = gh100_fsp_boot_gsp_fmc,
	},
};

int
gb202_fsp_new(struct nvkm_device *device,
	      enum nvkm_subdev_type type, int inst, struct nvkm_fsp **pfsp)
{
	return nvkm_fsp_new_(&gb202_fsp, device, type, inst, pfsp);
}
