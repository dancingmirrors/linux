/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */

#include <rm/rpc.h>

#include "nvrm/crashcat.h"

#include <core/option.h>

#include <linux/sizes.h>
#include <linux/unaligned.h>

#define FALCON_MAILBOX0			0x040
#define FALCON_MAILBOX1			0x044
#define FALCON_DEBUGINFO		0x094
#define FALCON_COMMON_SCRATCH_GROUP(n, i)	(0x300 + (n) * 0x10 + (i) * 4)

static const struct {
	u8 nr;
	u16 reg[4];
} r535_gsp_crashcat_scratch[] = {
	[NV_CRASHCAT_SCRATCH_GROUP_ID_A] = { 2, { FALCON_MAILBOX0, FALCON_MAILBOX1 } },
	[NV_CRASHCAT_SCRATCH_GROUP_ID_B] = { 4, { FALCON_COMMON_SCRATCH_GROUP(0, 0),
						  FALCON_COMMON_SCRATCH_GROUP(0, 1),
						  FALCON_COMMON_SCRATCH_GROUP(0, 2),
						  FALCON_COMMON_SCRATCH_GROUP(0, 3) } },
	[NV_CRASHCAT_SCRATCH_GROUP_ID_C] = { 4, { FALCON_COMMON_SCRATCH_GROUP(1, 0),
						  FALCON_COMMON_SCRATCH_GROUP(1, 1),
						  FALCON_COMMON_SCRATCH_GROUP(1, 2),
						  FALCON_COMMON_SCRATCH_GROUP(1, 3) } },
	[NV_CRASHCAT_SCRATCH_GROUP_ID_D] = { 4, { FALCON_COMMON_SCRATCH_GROUP(2, 0),
						  FALCON_COMMON_SCRATCH_GROUP(2, 1),
						  FALCON_COMMON_SCRATCH_GROUP(2, 2),
						  FALCON_COMMON_SCRATCH_GROUP(2, 3) } },
	[NV_CRASHCAT_SCRATCH_GROUP_ID_E] = { 4, { FALCON_COMMON_SCRATCH_GROUP(3, 0),
						  FALCON_COMMON_SCRATCH_GROUP(3, 1),
						  FALCON_COMMON_SCRATCH_GROUP(3, 2),
						  FALCON_COMMON_SCRATCH_GROUP(3, 3) } },
};

#define R535_GSP_CRASHCAT_QUEUE_MAX	SZ_64K

static u32
r535_gsp_crashcat_unit_bytes(u8 unit)
{
	switch (unit) {
	case NV_CRASHCAT_MEM_UNIT_SIZE_8B: return 8;
	case NV_CRASHCAT_MEM_UNIT_SIZE_1KB: return SZ_1K;
	case NV_CRASHCAT_MEM_UNIT_SIZE_4KB: return SZ_4K;
	case NV_CRASHCAT_MEM_UNIT_SIZE_64KB: return SZ_64K;
	default: return 0;
	}
}

static const char *
r535_gsp_crashcat_aperture(u8 aperture)
{
	switch (aperture) {
	case NV_CRASHCAT_MEM_APERTURE_SYSGPA: return "sysmem";
	case NV_CRASHCAT_MEM_APERTURE_FBGPA: return "vram";
	case NV_CRASHCAT_MEM_APERTURE_DMEM: return "DMEM";
	case NV_CRASHCAT_MEM_APERTURE_EMEM: return "EMEM";
	default: return "unknown";
	}
}

static const char *
r535_gsp_crashcat_mode(u8 mode, bool libos)
{
	switch (mode) {
	case NV_CRASHCAT_RISCV_MODE_M:
		return libos ? "monitor" : "M-mode";
	case NV_CRASHCAT_RISCV_MODE_S:
		return libos ? "kernel" : "S-mode";
	case NV_CRASHCAT_RISCV_MODE_U:
		return libos ? "task" : "U-mode";
	default:
		return "unspecified";
	}
}

static const char *
r535_gsp_crashcat_containment(u8 containment)
{
	switch (containment) {
	case NV_CRASHCAT_CONTAINMENT_RISCV_MODE_M:
		return "M-mode";
	case NV_CRASHCAT_CONTAINMENT_RISCV_MODE_S:
		return "S-mode";
	case NV_CRASHCAT_CONTAINMENT_RISCV_MODE_U:
		return "U-mode";
	case NV_CRASHCAT_CONTAINMENT_RISCV_HART:
		return "hart";
	case NV_CRASHCAT_CONTAINMENT_UNCONTAINED:
		return "uncontained";
	default:
		return "unspecified";
	}
}

static bool
r535_gsp_crashcat_fatal(u8 containment)
{
	switch (containment) {
	case NV_CRASHCAT_CONTAINMENT_RISCV_MODE_M:
	case NV_CRASHCAT_CONTAINMENT_RISCV_HART:
	case NV_CRASHCAT_CONTAINMENT_UNCONTAINED:
		return true;
	default:
		return false;
	}
}

static const char *
r535_gsp_crashcat_libos3_reason(u8 reason)
{
	switch (reason) {
	case LIBOS_PANIC_REASON_UNHANDLED_STATE: return "unhandled state";
	case LIBOS_PANIC_REASON_INVALID_CONFIGURATION: return "invalid configuration";
	case LIBOS_PANIC_REASON_FATAL_HARDWARE_ERROR: return "fatal hardware error";
	case LIBOS_PANIC_REASON_INSUFFICIENT_RESOURCES: return "insufficient resources";
	case LIBOS_PANIC_REASON_TIMEOUT: return "timeout";
	case LIBOS_PANIC_REASON_ENV_CALL_FAILED: return "environment call failed";
	case LIBOS_PANIC_REASON_ASAN_MEMORY_ERROR: return "asan memory error detected";
	case LIBOS_PANIC_REASON_PROGRAMMING_ERROR: return "programming error";
	case LIBOS_PANIC_REASON_ASSERTION_FAILED: return "condition failed";
	case LIBOS_PANIC_REASON_TRAP_KERNEL_PANIC: return "unhandled trap";
	case LIBOS_PANIC_REASON_TRAP_INSTRUCTION: return "instruction access fault";
	default: return "unknown error";
	}
}

static const char *
r535_gsp_crashcat_xcause(u64 xcause)
{
	const u8 code = xcause & 0x1f;

	if (xcause & BIT_ULL(63)) {
		switch (code) {
		case 0: return "user software interrupt";
		case 1: return "supervisor software interrupt";
		case 3: return "machine software interrupt";
		case 4: return "user timer interrupt";
		case 5: return "supervisor timer interrupt";
		case 7: return "machine timer interrupt";
		case 8: return "user external interrupt";
		case 9: return "supervisor external interrupt";
		case 11: return "machine external interrupt";
		default: return "unknown interrupt";
		}
	}

	switch (code) {
	case 0: return "instruction address misaligned";
	case 1: return "instruction access fault";
	case 2: return "illegal instruction";
	case 3: return "breakpoint";
	case 4: return "load address misaligned";
	case 5: return "load access fault";
	case 6: return "store address misaligned";
	case 7: return "store access fault";
	case 8: return "environment call from U-mode";
	case 9: return "environment call from S-mode";
	case 11: return "environment call from M-mode";
	case 12: return "instruction access page fault";
	case 13: return "load access page fault";
	case 15: return "store access page fault";
	default: return "unknown exception";
	}
}

static bool
r535_gsp_crashcat_log_report(struct nvkm_gsp *gsp, const u8 *p, u32 size)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	const u64 impl = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_IMPLEMENTER_SIGNATURE);
	const u64 reporter = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_REPORTER_ID);
	const u64 rdata = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_REPORTER_DATA);
	const u64 source = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_SOURCE_ID);
	const u64 cause = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_SOURCE_CAUSE);
	const u64 pc = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_SOURCE_PC);
	const u64 data = get_unaligned_le64(p + NV_CRASHCAT_REPORT_V1_SOURCE_DATA);
	const bool libos3 = impl == NV_CRASHCAT_REPORT_IMPLEMENTER_SIGNATURE_LIBOS3;
	const u8 type = NV_CRASHCAT_REPORT_V1_SOURCE_CAUSE_TYPE(cause);
	const u8 containment = NV_CRASHCAT_REPORT_V1_SOURCE_CAUSE_CONTAINMENT(cause);
	const u8 task = NV_CRASHCAT_REPORT_V1_ID_LIBOS3_TASK_ID(source);
	const char *mode = r535_gsp_crashcat_mode(NV_CRASHCAT_REPORT_V1_ID_RISCV_MODE(source),
						  libos3);
	const u32 version = NV_CRASHCAT_REPORT_V1_REPORTER_DATA_VERSION(rdata);
	char who[48];

	if (libos3 && task != NV_CRASHCAT_REPORT_V1_ID_LIBOS3_TASK_ID_UNSPECIFIED)
		snprintf(who, sizeof(who), "partition:%llu#%llu, task:%u",
			 NV_CRASHCAT_REPORT_V1_ID_NVRISCV_PARTITION(source),
			 NV_CRASHCAT_REPORT_V1_ID_NVRISCV_UCODE_ID(source), task);
	else
		snprintf(who, sizeof(who), "partition:%llu#%llu",
			 NV_CRASHCAT_REPORT_V1_ID_NVRISCV_PARTITION(source),
			 NV_CRASHCAT_REPORT_V1_ID_NVRISCV_UCODE_ID(source));

	nvkm_error(subdev, "****************************** GSP-CrashCat Report *******************************\n");

	switch (type) {
	case NV_CRASHCAT_CAUSE_TYPE_EXCEPTION:
		nvkm_error(subdev, "%s exception: %s (cause:0x%llx) @ pc:0x%llx, %s\n",
			   mode, r535_gsp_crashcat_xcause(data), data, pc, who);
		break;
	case NV_CRASHCAT_CAUSE_TYPE_TIMEOUT:
		nvkm_error(subdev, "%s timeout @ pc:0x%llx, %s\n", mode, pc, who);
		break;
	case NV_CRASHCAT_CAUSE_TYPE_PANIC:
		if (libos3) {
			const u8 reason = NV_CRASHCAT_REPORT_V1_SOURCE_CAUSE_LIBOS3_REASON(cause);

			nvkm_error(subdev, "%s panic: %s (%u) @ pc:0x%llx, aux:0x%llx, %s\n",
				   mode, r535_gsp_crashcat_libos3_reason(reason), reason,
				   pc, data, who);
		} else {
			nvkm_error(subdev, "%s panic @ pc:0x%llx, data:0x%llx, %s\n",
				   mode, pc, data, who);
		}
		break;
	default:
		nvkm_error(subdev, "%s unknown failure (0x%llx) @ pc:0x%llx, data:0x%llx, %s\n",
			   mode, cause, pc, data, who);
		break;
	}

	nvkm_error(subdev, "containment:%s (%u) implementer:%016llx\n",
		   r535_gsp_crashcat_containment(containment), containment, impl);

	if (libos3) {
		nvkm_error(subdev, "reported by libos partition:%llu#%llu task:%llu v%u.%u [%u] @ ts:%llu\n",
			   NV_CRASHCAT_REPORT_V1_ID_NVRISCV_PARTITION(reporter),
			   NV_CRASHCAT_REPORT_V1_ID_NVRISCV_UCODE_ID(reporter),
			   NV_CRASHCAT_REPORT_V1_ID_LIBOS3_TASK_ID(reporter),
			   NV_CRASHCAT_REPORT_V1_REPORTER_DATA_VERSION_LIBOS3_MAJOR(version),
			   NV_CRASHCAT_REPORT_V1_REPORTER_DATA_VERSION_LIBOS3_MINOR(version),
			   NV_CRASHCAT_REPORT_V1_REPORTER_DATA_VERSION_LIBOS3_CL(version),
			   NV_CRASHCAT_REPORT_V1_REPORTER_DATA_TIMESTAMP(rdata));
	} else {
		nvkm_error(subdev, "reported by partition:%llu ucode:%llu [%s] version:%u @ %llu\n",
			   NV_CRASHCAT_REPORT_V1_ID_NVRISCV_PARTITION(reporter),
			   NV_CRASHCAT_REPORT_V1_ID_NVRISCV_UCODE_ID(reporter),
			   r535_gsp_crashcat_mode(NV_CRASHCAT_REPORT_V1_ID_RISCV_MODE(reporter),
						  false),
			   version, NV_CRASHCAT_REPORT_V1_REPORTER_DATA_TIMESTAMP(rdata));
	}

	return r535_gsp_crashcat_fatal(containment);
}

static void
r535_gsp_crashcat_log_csr(struct nvkm_gsp *gsp, const u8 *p, u32 size, u32 meta)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	const u8 mode = NV_CRASHCAT_RISCV64_CSR_STATE_V1_HEADER_RISCV_MODE(meta);
	const char x = mode == NV_CRASHCAT_RISCV_MODE_M ? 'm' :
		       mode == NV_CRASHCAT_RISCV_MODE_S ? 's' : 'x';
	const u64 xcause = get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XCAUSE);

	nvkm_error(subdev, "RISC-V CSR state (%s):\n", r535_gsp_crashcat_mode(mode, false));
	nvkm_error(subdev, "  %cstatus:0x%016llx  %cscratch:0x%016llx  %cie:0x%016llx  %cip:0x%016llx\n",
		   x, get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XSTATUS),
		   x, get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XSCRATCH),
		   x, get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XIE),
		   x, get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XIP));
	nvkm_error(subdev, "  %cepc:0x%016llx  %ctval:0x%016llx  %ccause:0x%016llx (%s)\n",
		   x, get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XEPC),
		   x, get_unaligned_le64(p + NV_CRASHCAT_RISCV64_CSR_STATE_V1_XTVAL),
		   x, xcause, r535_gsp_crashcat_xcause(xcause));
}

static void
r535_gsp_crashcat_log_gpr(struct nvkm_gsp *gsp, const u8 *p, u32 size)
{
	static const char *const names[31] = {
		"ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
		"a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6",
		"s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
	};
	struct nvkm_subdev *subdev = &gsp->subdev;
	int i;

	nvkm_error(subdev, "RISC-V GPR state:\n");
	for (i = 0; i + 3 < 31; i += 4) {
		nvkm_error(subdev, "  %3s:0x%016llx %3s:0x%016llx %3s:0x%016llx %3s:0x%016llx\n",
			   names[i], get_unaligned_le64(p + i * 8),
			   names[i + 1], get_unaligned_le64(p + (i + 1) * 8),
			   names[i + 2], get_unaligned_le64(p + (i + 2) * 8),
			   names[i + 3], get_unaligned_le64(p + (i + 3) * 8));
	}
	nvkm_error(subdev, "  %3s:0x%016llx %3s:0x%016llx %3s:0x%016llx\n",
		   names[28], get_unaligned_le64(p + 28 * 8),
		   names[29], get_unaligned_le64(p + 29 * 8),
		   names[30], get_unaligned_le64(p + 30 * 8));
}

static void
r535_gsp_crashcat_log_trace(struct nvkm_gsp *gsp, const u8 *p, u32 size, u32 meta)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	const u32 nr = min_t(u32, size / 8, 64);
	u32 i;

	nvkm_error(subdev, "RISC-V %s trace (%s), %u entries:\n",
		   NV_CRASHCAT_RISCV64_TRACE_V1_HEADER_TRACE_TYPE(meta) ==
		   NV_CRASHCAT_TRACE_TYPE_STACK ? "stack" : "PC",
		   r535_gsp_crashcat_mode(NV_CRASHCAT_RISCV64_TRACE_V1_HEADER_RISCV_MODE(meta),
					  false), size / 8);
	for (i = 0; i < nr; i += 4) {
		char line[4 * 20 + 1] = "";
		u32 j;

		for (j = i; j < min(i + 4, nr); j++) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " 0x%016llx",
				 get_unaligned_le64(p + j * 8));
		}
		nvkm_error(subdev, " %s\n", line);
	}
}

static void
r535_gsp_crashcat_log_io32(struct nvkm_gsp *gsp, const u8 *p, u32 size, u32 meta)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	const u32 nr = min_t(u32, size / 8, 64);
	u32 i;

	nvkm_error(subdev, "IO32 state (aperture %u), %u registers:\n",
		   NV_CRASHCAT_IO32_STATE_V1_HEADER_APERTURE(meta), size / 8);
	for (i = 0; i < nr; i += 4) {
		char line[4 * 22 + 1] = "";
		u32 j;

		for (j = i; j < min(i + 4, nr); j++) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line),
				 " %08x:%08x", get_unaligned_le32(p + j * 8 + 4),
				 get_unaligned_le32(p + j * 8));
		}
		nvkm_error(subdev, " %s\n", line);
	}
}

static u32
r535_gsp_crashcat_log(struct nvkm_gsp *gsp, const u8 *buf, u32 len, bool *fatal)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	u32 pos = 0, reports = 0;

	while (pos + 8 <= len) {
		const u64 hdr = get_unaligned_le64(buf + pos);
		const u8 type = NV_CRASHCAT_PACKET_HEADER_V1_TYPE(hdr);
		const u32 meta = NV_CRASHCAT_PACKET_HEADER_V1_META(hdr);
		const u8 *p = buf + pos + 8;
		u32 size;

		if (NV_CRASHCAT_PACKET_HEADER_SIGNATURE(hdr) != NV_CRASHCAT_SIGNATURE)
			break;

		size = NV_CRASHCAT_PACKET_HEADER_PAYLOAD_UNIT_SIZE(hdr);
		size = (NV_CRASHCAT_PACKET_HEADER_PAYLOAD_SIZE(hdr) + 1) *
		       r535_gsp_crashcat_unit_bytes(size);
		if (!size || pos + 8 + size > len) {
			nvkm_error(subdev, "crashcat: truncated packet type %u (%u bytes)\n",
				   type, size);
			break;
		}

		if (NV_CRASHCAT_PACKET_HEADER_FORMAT_VERSION(hdr) != 1) {
			nvkm_error(subdev, "crashcat: unknown packet format %llu, skipping\n",
				   NV_CRASHCAT_PACKET_HEADER_FORMAT_VERSION(hdr));
		} else {
			switch (type) {
			case NV_CRASHCAT_PACKET_TYPE_REPORT:
				if (size >= NV_CRASHCAT_REPORT_V1_SIZE) {
					if (r535_gsp_crashcat_log_report(gsp, p, size))
						*fatal = true;
					reports++;
				}
				break;
			case NV_CRASHCAT_PACKET_TYPE_RISCV64_CSR_STATE:
				if (size >= NV_CRASHCAT_RISCV64_CSR_STATE_V1_SIZE)
					r535_gsp_crashcat_log_csr(gsp, p, size, meta);
				break;
			case NV_CRASHCAT_PACKET_TYPE_RISCV64_GPR_STATE:
				if (size >= NV_CRASHCAT_RISCV64_GPR_STATE_V1_SIZE)
					r535_gsp_crashcat_log_gpr(gsp, p, size);
				break;
			case NV_CRASHCAT_PACKET_TYPE_RISCV64_TRACE:
				r535_gsp_crashcat_log_trace(gsp, p, size, meta);
				break;
			case NV_CRASHCAT_PACKET_TYPE_IO32_STATE:
				r535_gsp_crashcat_log_io32(gsp, p, size, meta);
				break;
			default:
				nvkm_error(subdev, "crashcat: unknown packet type %u (%u bytes)\n",
					   type, size);
				break;
			}
		}

		pos += 8 + size;
	}

	if (reports) {
		nvkm_error(subdev, "------------[ end crash report ]------------\n");
		gsp->crashcat.reported = true;
	}

	return pos;
}

static int
r535_gsp_crashcat_read(struct nvkm_gsp *gsp, u32 offset, u8 *buf, u32 len)
{
	enum nvkm_falcon_mem type;

	switch (gsp->crashcat.aperture) {
	case NV_CRASHCAT_MEM_APERTURE_DMEM:
		type = DMEM;
		break;
	case NV_CRASHCAT_MEM_APERTURE_EMEM:
		type = EMEM;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return nvkm_falcon_pio_rd(&gsp->falcon, 0, type, gsp->crashcat.offset + offset,
				  buf, 0, len);
}

static int
r535_gsp_crashcat_consume(struct nvkm_gsp *gsp, bool whole)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct nvkm_falcon *falcon = &gsp->falcon;
	const u32 size = gsp->crashcat.size;
	u32 put = nvkm_falcon_rd32(falcon, gsp->crashcat.put_reg);
	u32 get = nvkm_falcon_rd32(falcon, gsp->crashcat.get_reg);
	bool fatal = false;
	u32 len, used;
	u8 *buf;
	int ret;

	if (get >= size || put >= size || ((get | put) & 3)) {
		nvkm_error(subdev, "crashcat: queue pointers out of range (put:0x%x get:0x%x size:0x%x)\n",
			   put, get, size);
		gsp->crashcat.unusable = true;
		return -EINVAL;
	}

	if (put == get && !(whole && !put))
		return 0;

	buf = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (put == get) {
		nvkm_debug(subdev, "crashcat: put pointer not published after acknowledging the wayfinder, scanning the whole queue\n");
		ret = r535_gsp_crashcat_read(gsp, 0, buf, size);
		len = size;
	} else if (put > get) {
		len = put - get;
		ret = r535_gsp_crashcat_read(gsp, get, buf, len);
	} else {
		len = size - get + put;
		ret = r535_gsp_crashcat_read(gsp, get, buf, size - get);
		if (ret == 0 && put)
			ret = r535_gsp_crashcat_read(gsp, 0, buf + size - get, put);
	}

	if (ret) {
		nvkm_error(subdev, "crashcat: failed to read the report queue, %d\n", ret);
		gsp->crashcat.unusable = true;
		goto done;
	}

	used = r535_gsp_crashcat_log(gsp, buf, len, &fatal);
	if (!used) {
		if (put != get) {
			nvkm_error(subdev, "crashcat: unrecognised data in the report queue (put:0x%x get:0x%x)\n",
				   put, get);
			print_hex_dump(KERN_ERR, "crashcat: ", DUMP_PREFIX_OFFSET, 16, 4,
				       buf, min_t(u32, len, 64), false);
			ret = -ENODATA;
		} else {
			nvkm_info(subdev, "crashcat: wayfinder published but the report queue is empty, nothing was crash-reported\n");
			print_hex_dump_debug("crashcat: ", DUMP_PREFIX_OFFSET, 16, 4,
					     buf, min_t(u32, len, 64), false);
			ret = 0;
		}
	} else {
		ret = fatal ? 2 : 1;
	}

	if (put != get) {
		nvkm_falcon_wr32(falcon, gsp->crashcat.get_reg, put);
	} else if (used) {
		nvkm_falcon_wr32(falcon, gsp->crashcat.put_reg, used % size);
		nvkm_falcon_wr32(falcon, gsp->crashcat.get_reg, used % size);
	}

done:
	kvfree(buf);
	return ret;
}

static int
r535_gsp_crashcat_locate(struct nvkm_gsp *gsp, u32 l0)
{
	struct nvkm_subdev *subdev = &gsp->subdev;
	struct nvkm_falcon *falcon = &gsp->falcon;
	const u8 loc = NV_CRASHCAT_WAYFINDER_L0_V1_WFL1_LOCATION(l0);
	u64 l1, offset, limit;
	u32 unit, i;
	int ret = 0;

	gsp->crashcat.unusable = true;

	if (NV_CRASHCAT_WAYFINDER_L0_VERSION(l0) != NV_CRASHCAT_WAYFINDER_VERSION_1) {
		nvkm_error(subdev, "crashcat: unsupported wayfinder version %u (0x%08x)\n",
			   NV_CRASHCAT_WAYFINDER_L0_VERSION(l0), l0);
		return -EOPNOTSUPP;
	}

	if (loc >= ARRAY_SIZE(r535_gsp_crashcat_scratch) || !r535_gsp_crashcat_scratch[loc].nr) {
		nvkm_error(subdev, "crashcat: unsupported wayfinder location %u (0x%08x)\n",
			   loc, l0);
		return -EOPNOTSUPP;
	}

	l1  = nvkm_falcon_rd32(falcon, r535_gsp_crashcat_scratch[loc].reg[0]);
	l1 |= (u64)nvkm_falcon_rd32(falcon, r535_gsp_crashcat_scratch[loc].reg[1]) << 32;

	unit = NV_CRASHCAT_WAYFINDER_L1_V1_QUEUE_UNIT_SIZE(l1);
	unit = unit == NV_CRASHCAT_MEM_UNIT_SIZE_8B ? 0 : r535_gsp_crashcat_unit_bytes(unit);
	offset = NV_CRASHCAT_WAYFINDER_L1_V1_QUEUE_OFFSET(l1);
	gsp->crashcat.aperture = NV_CRASHCAT_WAYFINDER_L1_V1_QUEUE_APERTURE(l1);
	gsp->crashcat.size = (NV_CRASHCAT_WAYFINDER_L1_V1_QUEUE_SIZE(l1) + 1) * unit;

	nvkm_warn(subdev, "crashcat: wayfinder 0x%08x 0x%016llx: report queue in %s at 0x%llx, 0x%x bytes\n",
		  l0, l1, r535_gsp_crashcat_aperture(gsp->crashcat.aperture),
		  offset, gsp->crashcat.size);

	switch (gsp->crashcat.aperture) {
	case NV_CRASHCAT_MEM_APERTURE_EMEM:
		limit = SZ_64K;
		break;
	case NV_CRASHCAT_MEM_APERTURE_DMEM:
		limit = falcon->data.limit ? falcon->data.limit : SZ_64K;
		break;
	default:
		limit = 0;
		break;
	}

	if (!unit || gsp->crashcat.size > R535_GSP_CRASHCAT_QUEUE_MAX ||
	    offset + gsp->crashcat.size > limit) {
		nvkm_error(subdev, "crashcat: can't read a report queue like that\n");
		return -EOPNOTSUPP;
	}

	gsp->crashcat.offset = offset;

	if (r535_gsp_crashcat_scratch[loc].nr >= 4) {
		gsp->crashcat.put_reg = r535_gsp_crashcat_scratch[loc].reg[2];
		gsp->crashcat.get_reg = r535_gsp_crashcat_scratch[loc].reg[3];
	} else {
		gsp->crashcat.put_reg = r535_gsp_crashcat_scratch[loc].reg[0];
		gsp->crashcat.get_reg = r535_gsp_crashcat_scratch[loc].reg[1];

		nvkm_falcon_wr32(falcon, gsp->crashcat.get_reg, 0);
		nvkm_falcon_wr32(falcon, gsp->crashcat.put_reg, 0);
		nvkm_falcon_wr32(falcon, FALCON_DEBUGINFO,
				 l0 & ~NV_CRASHCAT_WAYFINDER_L0_V1_WFL1_LOCATION_MASK);

		for (i = 0; i < 1000; i++) {
			if (nvkm_falcon_rd32(falcon, gsp->crashcat.put_reg))
				break;
			usleep_range(100, 200);
		}

		if (i == 1000)
			ret = 1;
	}

	gsp->crashcat.unusable = false;
	gsp->crashcat.valid = true;
	return ret;
}

bool
r535_gsp_crashcat_pending(struct nvkm_gsp *gsp)
{
	struct nvkm_falcon *falcon = &gsp->falcon;
	u32 l0 = nvkm_falcon_rd32(falcon, FALCON_DEBUGINFO);

	if (NV_CRASHCAT_WAYFINDER_L0_SIGNATURE(l0) != NV_CRASHCAT_SIGNATURE ||
	    gsp->crashcat.unusable || gsp->crashcat.disabled)
		return false;

	if (!gsp->crashcat.valid)
		return NV_CRASHCAT_WAYFINDER_L0_V1_WFL1_LOCATION(l0) !=
		       NV_CRASHCAT_SCRATCH_GROUP_ID_NONE;

	return nvkm_falcon_rd32(falcon, gsp->crashcat.put_reg) !=
	       nvkm_falcon_rd32(falcon, gsp->crashcat.get_reg);
}

int
r535_gsp_crashcat_check(struct nvkm_gsp *gsp)
{
	struct nvkm_falcon *falcon = &gsp->falcon;
	u32 l0 = nvkm_falcon_rd32(falcon, FALCON_DEBUGINFO);
	bool whole = false;
	int ret;

	if (NV_CRASHCAT_WAYFINDER_L0_SIGNATURE(l0) != NV_CRASHCAT_SIGNATURE ||
	    gsp->crashcat.unusable || gsp->crashcat.disabled)
		return 0;

	if (!gsp->crashcat.valid) {
		if (NV_CRASHCAT_WAYFINDER_L0_V1_WFL1_LOCATION(l0) ==
		    NV_CRASHCAT_SCRATCH_GROUP_ID_NONE)
			return 0;

		ret = r535_gsp_crashcat_locate(gsp, l0);
		if (ret < 0)
			return ret;

		whole = ret == 1;
	}

	return r535_gsp_crashcat_consume(gsp, whole);
}

const char *
r535_gsp_crashcat_status(struct nvkm_gsp *gsp)
{
	if (gsp->crashcat.disabled)
		return "CrashCat disabled";
	if (gsp->crashcat.unusable)
		return "CrashCat queue unusable";
	return "no CrashCat report";
}

void
r535_gsp_crashcat_reset(struct nvkm_gsp *gsp)
{
	gsp->crashcat.valid = false;
	gsp->crashcat.unusable = false;
	gsp->crashcat.reported = false;
	gsp->crashcat.disabled = !nvkm_boolopt(gsp->subdev.device->cfgopt, "NvCrashCat", true);
}
