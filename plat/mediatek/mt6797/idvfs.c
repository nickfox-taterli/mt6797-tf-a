/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

#include <drivers/delay_timer.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <lib/spinlock.h>
#include <lib/utils_def.h>

#include <mtk_sip_svc.h>
#include <platform_def.h>

/* PTP3/iDVFS register block used by the Vendor MT6797 firmware. */
#define MT6797_IDVFS_SRAM_LDO		(MCUCFG_BASE + 0x22b0)
#define MT6797_IDVFS_SRAM_LDO_CAL	(MCUCFG_BASE + 0x22b4)

/* Keep the frequency setter serialized with the SRAM LDO operation. */
static spinlock_t mt6797_idvfs_lock;

static int mt6797_idvfs_validate_freq(unsigned int freq_mhz)
{
	return (freq_mhz >= 250U && freq_mhz <= 3000U) ?
		MTK_SIP_E_SUCCESS : MTK_SIP_E_INVALID_PARAM;
}

int mt6797_idvfs_set_big_pll(unsigned int freq_mhz)
{
	uint32_t value;
	uint32_t freq_sdm_pcm;
	uint64_t pcw;
	unsigned int pos_div;
	int rc;

	rc = mt6797_idvfs_validate_freq(freq_mhz);
	if (rc != MTK_SIP_E_SUCCESS)
		return rc;

	if (freq_mhz <= 500U) {
		freq_sdm_pcm = freq_mhz * 4U;
		pos_div = 2U;
	} else if (freq_mhz <= 1000U) {
		freq_sdm_pcm = freq_mhz * 2U;
		pos_div = 1U;
	} else {
		freq_sdm_pcm = freq_mhz;
		pos_div = 0U;
	}

	spin_lock(&mt6797_idvfs_lock);

	/* This is the Vendor API_BIGPLLSETFREQ() ordering. */
	value = mmio_read_32(MT6797_BIG_ARMPLL_CON0) & ~BIT_32(0);
	mmio_write_32(MT6797_BIG_ARMPLL_CON0, value);
	udelay(1);

	value = mmio_read_32(MT6797_BIG_ARMPLL_CON0) & ~U(0x7 << 12);
	value |= (pos_div << 12);
	mmio_write_32(MT6797_BIG_ARMPLL_CON0, value);

	value |= BIT_32(0);
	mmio_write_32(MT6797_BIG_ARMPLL_CON0, value);
	udelay(1);

	pcw = ((uint64_t)U(1) << 24) * freq_sdm_pcm / 26U;
	value = mmio_read_32(MT6797_BIG_ARMPLL_CON1) & ~U(0x7fffffff);
	value |= (uint32_t)pcw & U(0x7fffffff);
	mmio_write_32(MT6797_BIG_ARMPLL_CON1, value);
	udelay(1);

	value = mmio_read_32(MT6797_BIG_ARMPLL_CON1) | BIT_32(31);
	mmio_write_32(MT6797_BIG_ARMPLL_CON1, value);
	udelay(1);
	value &= ~BIT_32(31);
	mmio_write_32(MT6797_BIG_ARMPLL_CON1, value);
	udelay(20);

	spin_unlock(&mt6797_idvfs_lock);
	return MTK_SIP_E_SUCCESS;
}

int mt6797_idvfs_set_big_sram_ldo(unsigned int mv_x100)
{
	uint32_t value;

	if (mv_x100 < 60000U || mv_x100 > 120000U)
		return MTK_SIP_E_INVALID_PARAM;

	spin_lock(&mt6797_idvfs_lock);

	/* Use the eFuse calibration value when present, as Vendor does. */
	value = mmio_read_32(UL(0x1020666c)) & U(0xffff);
	mmio_write_32(MT6797_IDVFS_SRAM_LDO_CAL,
			value == 0U ? U(0x00007777) : value);

	value = (mmio_read_32(MT6797_IDVFS_SRAM_LDO) & U(0xfffff000)) |
		U(0x8f0);
	if (mv_x100 < 70000U)
		value |= U(1);
	else if (mv_x100 < 90000U)
		value |= U(2);
	else
		value |= (((mv_x100 - 90000U) / 2500U) + 3U);

	mmio_write_32(MT6797_IDVFS_SRAM_LDO, value);

	spin_unlock(&mt6797_idvfs_lock);
	return MTK_SIP_E_SUCCESS;
}
