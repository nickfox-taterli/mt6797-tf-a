/*
 * Copyright (c) 2026, Arm Limited and Contributors.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <common/debug.h>
#include <arch_helpers.h>
#include <lib/mmio.h>
#include <lib/spinlock.h>
#include <lib/utils_def.h>
#include <platform_def.h>

#include <mtk_sip_svc.h>

#include "mt6797_emi_mpu.h"

#define MT6797_EMI_MPUC3		(EMI_MPU_BASE + 0x0370U)
#define MT6797_EMI_MPUD3		(EMI_MPU_BASE + 0x0378U)
#define MT6797_EMI_MPUK3		(EMI_MPU_BASE + 0x03b0U)
#define MT6797_EMI_MPUL3		(EMI_MPU_BASE + 0x03b8U)

#define MT6797_EMI_MPU_ALIGNMENT	UL(0x10000)
#define MT6797_EMI_WINDOW_SIZE		UL(0x80000)
#define MT6797_EMI_CARVEOUT_ALIGNMENT	UL(0x200000)
#define MT6797_EMI_DRAM_BASE		UL(0x40000000)
#define MT6797_EMI_DRAM_LIMIT		UL(0x100000000)

#define MT6797_EMI_REGION_SHIFT		27U
#define MT6797_EMI_LOCK_SHIFT		26U
#define MT6797_EMI_REGION18		18U
#define MT6797_EMI_REGION19		19U
#define MT6797_EMI_PERMISSION_MASK	U(0x00ffffff)

#define MT6797_EMI_PERMISSION(d7, d6, d5, d4, d3, d2, d1, d0) \
	((((uint32_t)(d7)) << 21) | (((uint32_t)(d6)) << 18) | \
	 (((uint32_t)(d5)) << 15) | (((uint32_t)(d4)) << 12) | \
	 (((uint32_t)(d3)) << 9) | (((uint32_t)(d2)) << 6) | \
	 (((uint32_t)(d1)) << 3) | ((uint32_t)(d0)))

/* Exact permission words used by the vendor MT6797 WLAN/ConSys paths. */
#define MT6797_EMI_REGION18_OPEN	U(0x00000000)
#define MT6797_EMI_REGION18_RESTORE	U(0x00b6da2d)
#define MT6797_EMI_REGION19_HOST	U(0x00b6da28)

/* The legacy implementation subtracts this alias before programming 64K units. */
#define MT6797_EMI_DRAM_ALIAS_REG	UL(0x10001f00)
#define MT6797_EMI_DRAM_4GB_BIT		BIT(13)

static spinlock_t mt6797_emi_mpu_lock;

static int mt6797_emi_valid_window(uint64_t start, uint64_t end,
					   unsigned int region)
{
	uint64_t expected_offset;

	if (start < MT6797_EMI_DRAM_BASE || end >= MT6797_EMI_DRAM_LIMIT ||
	    end < start || (start & (MT6797_EMI_MPU_ALIGNMENT - 1U)) != 0U ||
	    ((end + 1U) & (MT6797_EMI_MPU_ALIGNMENT - 1U)) != 0U ||
	    end - start + 1U != MT6797_EMI_WINDOW_SIZE)
		return MTK_SIP_E_INVALID_RANGE;

	/* The DT allocator supplies one 2 MiB-aligned 2 MiB carveout. */
	expected_offset = region == MT6797_EMI_REGION18 ? 0U : MT6797_EMI_WINDOW_SIZE;
	if ((start & (MT6797_EMI_CARVEOUT_ALIGNMENT - 1U)) != expected_offset)
		return MTK_SIP_E_INVALID_RANGE;

	return MTK_SIP_E_SUCCESS;
}

int mt6797_emi_mpu_set_region(uint64_t start, uint64_t end,
				      uint32_t region_permission)
{
	unsigned int region = (region_permission >> MT6797_EMI_REGION_SHIFT) & 0x1fU;
	uint32_t permission = region_permission & MT6797_EMI_PERMISSION_MASK;
	uint64_t emi_offset;
	uint32_t range;
	uintptr_t permission_reg;
	uintptr_t range_reg;
	int ret;

	if (region != MT6797_EMI_REGION18 && region != MT6797_EMI_REGION19)
		return MTK_SIP_E_INVALID_PARAM;
	if (region_permission & BIT(MT6797_EMI_LOCK_SHIFT))
		return MTK_SIP_E_PERMISSION_DENY;
	if (region_permission & U(0x03000000))
		return MTK_SIP_E_INVALID_PARAM;

	if (region == MT6797_EMI_REGION18) {
		if (permission != MT6797_EMI_REGION18_OPEN &&
		    permission != MT6797_EMI_REGION18_RESTORE)
			return MTK_SIP_E_INVALID_PARAM;
	} else if (permission != MT6797_EMI_REGION19_HOST) {
		return MTK_SIP_E_INVALID_PARAM;
	}

	ret = mt6797_emi_valid_window(start, end, region);
	if (ret)
		return ret;

	/* Match the vendor ATF's 4-GiB-mode handling without exposing its raw SMC. */
	emi_offset = (mmio_read_32(MT6797_EMI_DRAM_ALIAS_REG) &
			      MT6797_EMI_DRAM_4GB_BIT) ? 0U : MT6797_EMI_DRAM_BASE;
	if (start < emi_offset || end < emi_offset)
		return MTK_SIP_E_INVALID_RANGE;
	start = (start - emi_offset) >> 16;
	end = (end - emi_offset) >> 16;
	if (start > 0xffffU || end > 0xffffU || end < start)
		return MTK_SIP_E_INVALID_RANGE;
	range = ((uint32_t)start << 16) | (uint32_t)end;

	permission_reg = region == MT6797_EMI_REGION18 ? MT6797_EMI_MPUK3 :
							     MT6797_EMI_MPUL3;
	range_reg = region == MT6797_EMI_REGION18 ? MT6797_EMI_MPUC3 :
							 MT6797_EMI_MPUD3;

	spin_lock(&mt6797_emi_mpu_lock);
	/* Vendor ordering: clear APC, program range, then restore APC. */
	mmio_write_32(permission_reg, 0U);
	mmio_write_32(range_reg, range);
	dsb();
	mmio_write_32(permission_reg, permission);
	dsb();
	spin_unlock(&mt6797_emi_mpu_lock);

	INFO("MT6797 EMI region %u: %llx-%llx permission=0x%06x\n",
	     region, (unsigned long long)(start << 16),
	     (unsigned long long)((end << 16) | 0xffffU), permission);
	return MTK_SIP_E_SUCCESS;
}

uint32_t mt6797_emi_mpu_read(uint32_t offset)
{
	switch (offset) {
	case 0x0370U:
	case 0x0378U:
	case 0x03b0U:
	case 0x03b8U:
		return mmio_read_32(EMI_MPU_BASE + offset);
	default:
		return U(0xffffffff);
	}
}
