/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include <arch_helpers.h>
#include <bl31/bl31.h>
#include <common/debug.h>
#include <common/runtime_svc.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <lib/smccc.h>
#include <tools_share/uuid.h>

#include <mtk_sip_svc.h>
#include <mtk_plat_common.h>
#include <plat_private.h>
#include <plat_sip_calls.h>
#include <platform_def.h>

#include "mt6797_emi_mpu.h"

extern int mt6797_idvfs_set_big_pll(unsigned int freq_mhz);

/* Keep the UUID used by the vendor MediaTek SiP service. */
DEFINE_SVC_UUID2(mt6797_sip_svc_uid,
	0xa42b58f7, 0x6242, 0x7d4d, 0x80, 0xe5,
	0x8f, 0x95, 0x05, 0x00, 0x0f, 0x3d);

struct mt6797_kernel_info {
	uint64_t pc;
	uint64_t r0;
	uint64_t r1;
	uint64_t r2;
	uint64_t k32_64;
};

static struct mt6797_kernel_info kernel_info;

static void mt6797_save_kernel_info(uint64_t pc, uint64_t r0,
				    uint64_t r1, uint64_t r2,
				    uint64_t k32_64)
{
	kernel_info.pc = pc;
	kernel_info.k32_64 = k32_64;

	if (k32_64 == 0U) {
		/* AArch32 Linux uses r0=0, r1=machtype, r2=tags. */
		kernel_info.r0 = 0U;
		kernel_info.r1 = r0;
		kernel_info.r2 = r1;
	} else {
		/* AArch64 Linux receives the DTB in x0; x1 is reserved. */
		kernel_info.r0 = r0;
		kernel_info.r1 = r1;
		kernel_info.r2 = r2;
	}
}

uint64_t get_kernel_info_pc(void)
{
	return kernel_info.pc;
}

uint64_t get_kernel_info_r0(void)
{
	return kernel_info.r0;
}

uint64_t get_kernel_info_r1(void)
{
	return kernel_info.r1;
}

uint64_t get_kernel_info_r2(void)
{
	return kernel_info.r2;
}

/* One normalized SiP argument bundle: x1..x4 as seen by the dispatch. */
struct mt6797_sip_args {
	u_register_t x1;
	u_register_t x2;
	u_register_t x3;
	u_register_t x4;
};

/* The vendor SiP ABI truncates every argument to 32 bits for AArch32
 * callers; AArch64 callers keep the full registers. */
static struct mt6797_sip_args mt6797_sip_normalize_args(uint32_t smc_fid,
					struct mt6797_sip_args args)
{
	if (GET_SMC_CC(smc_fid) == SMC_32) {
		args.x1 = (uint32_t)args.x1;
		args.x2 = (uint32_t)args.x2;
		args.x3 = (uint32_t)args.x3;
		args.x4 = (uint32_t)args.x4;
	}
	return args;
}

static bool mt6797_sip_valid_ocp_address(u_register_t address)
{
	return address >= MT6797_SIP_OCP_BASE &&
	       address < (MT6797_SIP_OCP_BASE + MT6797_SIP_OCP_SIZE) &&
	       (address & 0x3U) == 0U;
}

static bool mt6797_sip_valid_mcusys_write(u_register_t address)
{
	/* The mainline MT6797 SMP path only needs MP0 AXI ACINACTM. */
	return address == MT6797_MP0_AXI_CONFIG;
}

/* Kernel handoff: raise the shared rails, record the entry state and select
 * the AArch32/AArch64 entry ABI.  Returns no registers, as the vendor ABI
 * expects the boot registers to come from the saved kernel info. */
static uintptr_t mt6797_sip_kernel_boot(void *handle,
					struct mt6797_sip_args args)
{
	if (mt6797_kernel_power_prepare() != PSCI_E_SUCCESS)
		SMC_RET1(handle, PSCI_E_INTERN_FAIL);
	mt6797_save_kernel_info(args.x1, args.x2, args.x3, 0U, args.x4);
	bl31_prepare_kernel_entry(args.x4);
	SMC_RET0(handle);
}

/* Quiesce write for MP0 AXI ACINACTM, used by the SMP bring-up path. */
static uintptr_t mt6797_sip_mcusys_write(void *handle,
					 struct mt6797_sip_args args)
{
	if (!mt6797_sip_valid_mcusys_write(args.x1) || (args.x1 & 0x3U) != 0U) {
		SMC_RET1(handle, MTK_SIP_E_INVALID_RANGE);
	}
	mmio_write_32((uintptr_t)args.x1, (uint32_t)args.x2);
	dsb();
	SMC_RET1(handle, MTK_SIP_E_SUCCESS);
}

static uintptr_t mt6797_sip_ocp_read(void *handle, u_register_t address)
{
	if (!mt6797_sip_valid_ocp_address(address)) {
		SMC_RET1(handle, MTK_SIP_E_INVALID_RANGE);
	}
	SMC_RET1(handle, mmio_read_32((uintptr_t)address));
}

static uintptr_t mt6797_sip_ocp_write(void *handle, u_register_t address,
				      u_register_t value)
{
	if (!mt6797_sip_valid_ocp_address(address)) {
		SMC_RET1(handle, MTK_SIP_E_INVALID_RANGE);
	}
	mmio_write_32((uintptr_t)address, (uint32_t)value);
	SMC_RET1(handle, MTK_SIP_E_SUCCESS);
}

static uintptr_t mt6797_sip_handler(uint32_t smc_fid,
				    u_register_t x1,
				    u_register_t x2,
				    u_register_t x3,
				    u_register_t x4,
				    void *cookie,
				    void *handle,
				    u_register_t flags)
{
	struct mt6797_sip_args args;

	(void)cookie;

	if (!is_caller_non_secure(flags))
		SMC_RET1(handle, MTK_SIP_E_PERMISSION_DENY);

	args = mt6797_sip_normalize_args(smc_fid,
			(struct mt6797_sip_args){ x1, x2, x3, x4 });

	switch (smc_fid) {
#ifdef X20_WDT_FIQ
	case MT6797_SIP_WDT_CAPTURE_AARCH64:
		SMC_RET1(handle, mt6797_wdt_capture_control(args.x1, args.x2));
#endif
	case MT6797_SIP_TOPRGU_UPDATE_AARCH64:
		if (args.x1 >= 18U || args.x2 > 1U)
			SMC_RET1(handle, PSCI_E_INVALID_PARAMS);
		SMC_RET1(handle, mt6797_toprgu_update((unsigned int)args.x1,
							args.x2 != 0U));
	case MT6797_SIP_SLEEP_CAPS_AARCH64:
		SMC_RET2(handle, U(0x01010001), U(1));
	case MT6797_SIP_BOOT_CAPS_AARCH64:
		SMC_RET4(handle, MT6797_BOOT_CAPS_MAGIC, MT6797_BOOT_CAPS_ABI,
			 MT6797_BOOT_CAPS, mt6797_kernel_power_ready());

	case SIP_SVC_CALL_COUNT:
		SMC_RET1(handle, MT6797_SIP_NUM_CALLS);

	case SIP_SVC_UID:
		SMC_UUID_RET(handle, mt6797_sip_svc_uid);

	case SIP_SVC_VERSION:
		SMC_RET2(handle, MTK_SIP_SVC_VERSION_MAJOR,
			 MTK_SIP_SVC_VERSION_MINOR);

	case MT6797_SIP_KERNEL_BOOT_AARCH32:
	case MT6797_SIP_KERNEL_BOOT_AARCH64:
		return mt6797_sip_kernel_boot(handle, args);

	case MT6797_SIP_KERNEL_MCUSYS_WRITE_AARCH32:
	case MT6797_SIP_KERNEL_MCUSYS_WRITE_AARCH64:
		return mt6797_sip_mcusys_write(handle, args);

	case MT6797_SIP_OCP_READ_AARCH32:
	case MT6797_SIP_OCP_READ_AARCH64:
		return mt6797_sip_ocp_read(handle, args.x1);

	case MT6797_SIP_OCP_WRITE_AARCH32:
	case MT6797_SIP_OCP_WRITE_AARCH64:
		return mt6797_sip_ocp_write(handle, args.x1, args.x2);

	case MT6797_SIP_KERNEL_EMIMPU_READ_AARCH32:
	case MT6797_SIP_KERNEL_EMIMPU_READ_AARCH64:
		SMC_RET1(handle, mt6797_emi_mpu_read((uint32_t)args.x1));

	case MT6797_SIP_KERNEL_EMIMPU_SET_AARCH32:
	case MT6797_SIP_KERNEL_EMIMPU_SET_AARCH64:
		SMC_RET1(handle, mt6797_emi_mpu_set_region(args.x1, args.x2,
							   (uint32_t)args.x3));

	case MT6797_SIP_IDVFS_PLL_SET_FREQ_AARCH32:
	case MT6797_SIP_IDVFS_PLL_SET_FREQ_AARCH64:
		SMC_RET1(handle, mt6797_idvfs_set_big_pll((unsigned int)args.x1));

	case MT6797_SIP_IDVFS_SRAM_LDO_SET_AARCH32:
	case MT6797_SIP_IDVFS_SRAM_LDO_SET_AARCH64:
		SMC_RET1(handle,
			mt6797_idvfs_set_big_sram_ldo((unsigned int)args.x1));

	default:
		ERROR("MT6797 SiP: unhandled SMC 0x%x\n", smc_fid);
		SMC_RET1(handle, SMC_UNK);
	}
}

DECLARE_RT_SVC(
	mt6797_sip_svc,
	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_FAST,
	NULL,
	mt6797_sip_handler
);
