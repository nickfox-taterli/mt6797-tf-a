/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>
#include <stdint.h>

#include <arch_helpers.h>
#include <bl31/bl31.h>
#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <lib/spinlock.h>
#include <lib/utils_def.h>
#include <plat/common/platform.h>

#include <plat_private.h>
#include <platform_def.h>

#define MT6797_POWER_WAIT_LOOPS		U(100000)
#define MT6797_CCI_SNOOP_ENABLE		U(0x3)
#define MT6797_BIG_POWER_RETRIES		U(3)
#define MT6797_BIG_SPMC_ACK		BIT_32(17)

/* CPU0 is already running when BL31 is entered from the vendor boot chain. */
static unsigned int little_on = BIT_32(0);
static unsigned int big_on;
static bool big_cluster_on;
/* All peer-side SPM/CCI/INFRA RMW operations share one lock. */
static spinlock_t power_lock;
/* Bootstrap is never repeated by CPU_ON, hotplug or suspend/resume. */
static bool kernel_power_ready;
/* Written after the target has disabled caches and exited coherency. */
static uint32_t off_pending[PLATFORM_CORE_COUNT]
	__attribute__((section(".tzfw_coherent_mem")));

static int mt6797_toprgu_update_locked(unsigned int id, bool assert)
{
	uint32_t mask;
	uint32_t value;

	if (id > 17U)
		return PSCI_E_INVALID_PARAMS;

	mask = BIT_32(id);
	value = mmio_read_32(MT6797_WDT_SWSYSRST) | U(0x88000000);
	if (assert)
		value |= mask;
	else
		value &= ~mask;
	mmio_write_32(MT6797_WDT_SWSYSRST, value);

	value = mmio_read_32(MT6797_WDT_SWSYSRST);
	if ((value & mask) != (assert ? mask : 0U)) {
		ERROR("MT6797 power: TOPRGU bit%u %s readback 0x%x\n",
		      id, assert ? "set" : "clear", value);
		return PSCI_E_INTERN_FAIL;
	}
	return PSCI_E_SUCCESS;
}

int mt6797_toprgu_update(unsigned int id, bool assert)
{
	int rc;

	spin_lock(&power_lock);
	rc = mt6797_toprgu_update_locked(id, assert);
	spin_unlock(&power_lock);
	return rc;
}

bool mt6797_kernel_power_ready(void)
{
	return kernel_power_ready;
}

int mt6797_kernel_power_prepare(void)
{
	int rc, cleanup;

	/* LK's CPU0 handoff is the sole owner of raw I2C6. After this returns,
	 * Linux owns the controller and both always-on external voltage rails.
	 * Lock order: power_lock -> idvfs_lock; never call the TOPRGU wrapper.
	 */
	if (plat_my_core_pos() != 0U)
		return PSCI_E_DENIED;
	spin_lock(&power_lock);
	if (kernel_power_ready) {
		rc = PSCI_E_SUCCESS;
		goto out;
	}
	mmio_setbits_32(SPM_MP2_CPUSYS_PWR_CON, BIT_32(0));
	(void)mmio_read_32(MT6797_BIG_ARMPLL_CON0);
	rc = mt6797_toprgu_update_locked(11U, true);
	if (rc != PSCI_E_SUCCESS)
		goto out;
	rc = mt6797_boot_buck_enable();
	if (rc == PSCI_E_SUCCESS) {
		udelay(1000);
		mmio_clrbits_32(SPM_CPU_EXT_BUCK_ISO, U(0x3));
	}
	cleanup = mt6797_toprgu_update_locked(11U, false);
	if (rc == PSCI_E_SUCCESS)
		rc = cleanup;
	if (rc != PSCI_E_SUCCESS)
		goto out;
	udelay(240);
	rc = mt6797_idvfs_set_big_sram_ldo(110000U);
	udelay(240);
	if (rc == PSCI_E_SUCCESS)
		kernel_power_ready = true;
out:
	spin_unlock(&power_lock);
	if (rc != PSCI_E_SUCCESS)
		ERROR("X20 Linux power bootstrap failed: %d\n", rc);
	return rc;
}

static int mt6797_wait_mask(uintptr_t address, uint32_t mask, bool set)
{
	for (unsigned int i = 0U; i < MT6797_POWER_WAIT_LOOPS; ++i) {
		uint32_t value = mmio_read_32(address);

		if (set ? (value & mask) == mask : (value & mask) == 0U)
			return PSCI_E_SUCCESS;

		udelay(1);
	}

	ERROR("MT6797 power: timeout at 0x%lx mask 0x%x (%s)\n",
	      (unsigned long)address, mask, set ? "set" : "clear");
	return PSCI_E_INTERN_FAIL;
}

static int mt6797_wait_cluster_ack(uintptr_t address)
{
	for (unsigned int i = 0U; i < MT6797_POWER_WAIT_LOOPS; ++i) {
		uint32_t value = mmio_read_32(address);

		if ((value & BIT_32(2)) != 0U && (value & BIT_32(3)) != 0U)
			return PSCI_E_SUCCESS;

		udelay(1);
	}

	ERROR("MT6797 power: cluster sequence timeout at 0x%lx\n",
	      (unsigned long)address);
	return PSCI_E_INTERN_FAIL;
}

static void mt6797_spm_set_bit(uintptr_t address, unsigned int bit,
				       bool set)
{
	uint32_t value = mmio_read_32(address);

	if (set)
		value |= BIT_32(bit);
	else
		value &= ~BIT_32(bit);

	mmio_write_32(address, value);
}

static void mt6797_little_spark2_setldo(unsigned int linear_id)
{
	uintptr_t address;
	unsigned int offset;
	uint32_t value;

	if (linear_id < 4U) {
		address = MT6797_MP0_SPARKVRETCNTRL;
		offset = linear_id;
	} else {
		address = MT6797_MP1_SPARKVRETCNTRL;
		offset = linear_id - 4U;
	}

	value = mmio_read_32(address);
	value &= ~(U(0x3f) << (offset * 8U));
	value |= U(0x3f) << (offset * 8U);
	mmio_write_32(address, value);
}

static void mt6797_little_spark2_enable(unsigned int linear_id)
{
	uintptr_t address = linear_id < 4U ? MT6797_MP0_SPARKEN :
						 MT6797_MP1_SPARKEN;
	unsigned int bit = linear_id < 4U ? linear_id : linear_id - 4U;

	mt6797_spm_set_bit(address, bit, true);
}

bool mt6797_little_cluster_has_core(unsigned int cluster_id)
{
	unsigned int mask;

	if (cluster_id > 1U)
		return false;

	mask = cluster_id == 0U ? U(0x0f) : U(0xf0);
	return (little_on & mask) != 0U;
}

int mt6797_power_on_little_cluster(unsigned int cluster_id)
{
	static const unsigned int sequence_bits[] = { 0U, 4U, 2U, 3U, 1U,
		8U, 6U, 5U, 4U, 0U };
	static const bool sequence_values[] = { false, true, true, true, false,
		false, true, false, false, true };
	uintptr_t address;
	unsigned int protect_mask;
	int rc;

	if (cluster_id > 1U)
		return PSCI_E_INVALID_PARAMS;

	address = SPM_BASE + 0x210U + cluster_id * 4U;
	for (unsigned int i = 0U; i < ARRAY_SIZE(sequence_bits); ++i) {
		mt6797_spm_set_bit(address, sequence_bits[i], sequence_values[i]);

		if (i == 2U)
			udelay(1);
		else if (i == 3U) {
			rc = mt6797_wait_cluster_ack(address);
			if (rc != PSCI_E_SUCCESS)
				return rc;
			udelay(100);
		} else if (i == 5U) {
			rc = mt6797_wait_mask(address, BIT_32(12), false);
			if (rc != PSCI_E_SUCCESS)
				return rc;
			udelay(500);
		} else if (i == 6U) {
			udelay(1);
		}
	}

	if (cluster_id == 0U) {
		protect_mask = BIT_32(0) | BIT_32(4) | BIT_32(8) | BIT_32(11);
		mmio_write_32(MT6797_INFRA_PROTECTEN1,
				mmio_read_32(MT6797_INFRA_PROTECTEN1) & ~protect_mask);
		dsb();
		rc = mt6797_wait_mask(MT6797_INFRA_PROTECTSTA3, protect_mask,
					false);
		if (rc != PSCI_E_SUCCESS)
			return rc;

		mmio_write_32(MT6797_MP0_AXI_CONFIG,
				mmio_read_32(MT6797_MP0_AXI_CONFIG) & ~BIT_32(4));
		mmio_write_32(MT6797_CCI_SI4_SNOOP_CTRL,
				mmio_read_32(MT6797_CCI_SI4_SNOOP_CTRL) |
				MT6797_CCI_SNOOP_ENABLE);
	} else {
		static const unsigned int protect_bits[] = { 9U, 1U, 5U };

		for (unsigned int i = 0U; i < ARRAY_SIZE(protect_bits); ++i) {
			uint32_t bit = BIT_32(protect_bits[i]);

			mmio_write_32(MT6797_INFRA_PROTECTEN1,
					mmio_read_32(MT6797_INFRA_PROTECTEN1) & ~bit);
			dsb();
			rc = mt6797_wait_mask(MT6797_INFRA_PROTECTSTA3, bit, false);
			if (rc != PSCI_E_SUCCESS)
				return rc;
		}

		mmio_write_32(MT6797_MP1_AXI_CONFIG,
				mmio_read_32(MT6797_MP1_AXI_CONFIG) & ~BIT_32(4));
		mmio_write_32(MT6797_CCI_SI3_SNOOP_CTRL,
				mmio_read_32(MT6797_CCI_SI3_SNOOP_CTRL) |
				MT6797_CCI_SNOOP_ENABLE);
	}

	dsb();
	return mt6797_wait_mask(MT6797_CCI_STATUS, BIT_32(0), false);
}

int mt6797_power_on_little_core(unsigned int linear_id)
{
	static const unsigned int sequence_bits[] = { 16U, 0U, 4U, 8U, 9U, 8U,
		2U, 3U, 16U, 6U, 1U, 5U, 4U, 0U };
	static const bool sequence_values[] = { false, false, true, true, false,
		false, true, true, true, true, false, false, false, true };
	uintptr_t address;
	uint32_t status_mask;
	int rc;

	if (linear_id < 1U || linear_id > 7U)
		return PSCI_E_INVALID_PARAMS;
	if ((little_on & BIT_32(linear_id)) != 0U)
		return PSCI_E_SUCCESS;

	address = SPM_BASE + 0x220U + linear_id * 4U;
	for (unsigned int i = 0U; i < ARRAY_SIZE(sequence_bits); ++i) {
		mt6797_spm_set_bit(address, sequence_bits[i], sequence_values[i]);

		if (i == 3U) {
			rc = mt6797_wait_mask(address, BIT_32(12), true);
			if (rc != PSCI_E_SUCCESS)
				return rc;
		} else if (i == 5U) {
			rc = mt6797_wait_mask(address, BIT_32(12), false);
			if (rc != PSCI_E_SUCCESS)
				return rc;
		} else if (i == 6U) {
			udelay(1);
			status_mask = BIT_32(15U - linear_id);
			rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS, status_mask, true);
			if (rc != PSCI_E_SUCCESS)
				return rc;
		} else if (i == 7U) {
			udelay(1);
			status_mask = BIT_32(15U - linear_id);
			rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS_2ND,
					status_mask, true);
			if (rc != PSCI_E_SUCCESS)
				return rc;
		} else if (i == 10U) {
			udelay(1);
		}
	}

	mt6797_little_spark2_setldo(linear_id);
	mt6797_little_spark2_enable(linear_id);
	little_on |= BIT_32(linear_id);
	return PSCI_E_SUCCESS;
}

static int mt6797_big_sem_get(void)
{
	mmio_write_32(MT6797_BIG_HW_SEM_ENABLE, U(0x0b160001));

	for (unsigned int i = 0U; i < MT6797_POWER_WAIT_LOOPS; ++i) {
		mmio_write_32(MT6797_BIG_HW_SEM, U(1));
		if ((mmio_read_32(MT6797_BIG_HW_SEM) & BIT_32(0)) != 0U)
			return PSCI_E_SUCCESS;
		udelay(1);
	}

	ERROR("MT6797 power: big HW semaphore timeout\n");
	return PSCI_E_INTERN_FAIL;
}

static void mt6797_big_sem_put(void)
{
	mmio_write_32(MT6797_BIG_HW_SEM, U(1));
}

static int mt6797_big_measure_rate(uint32_t *rate_khz)
{
	uint32_t value;

	if (mt6797_big_sem_get() != PSCI_E_SUCCESS)
		return PSCI_E_INTERN_FAIL;

	mmio_write_32(MT6797_BIG_ARMPLL_K1, U(0));
	mmio_write_32(MT6797_BIG_ARM_DIV_MON_EN, U(0xffffffff));
	udelay(1);
	mt6797_big_sem_put();

	value = mmio_read_32(MT6797_BIG_CLK_DBG_CFG);
	mmio_write_32(MT6797_BIG_CLK_DBG_CFG,
			(value & U(0xffc0fffc)) | U(37 << 16));
	value = mmio_read_32(MT6797_BIG_CLK_MISC_CFG_0);
	mmio_write_32(MT6797_BIG_CLK_MISC_CFG_0,
			(value & U(0x00ffffff)) | U(0x01000000));
	mmio_write_32(MT6797_BIG_CLK26CALI_0, U(0x1000));
	mmio_write_32(MT6797_BIG_CLK26CALI_0, U(0x1010));

	for (unsigned int i = 0U; i <= 100U; ++i) {
		if ((mmio_read_32(MT6797_BIG_CLK26CALI_0) & BIT_32(4)) == 0U) {
			value = mmio_read_32(MT6797_BIG_CLK26CALI_1) & U(0xffff);
			*rate_khz = ((value * U(26000)) / U(1024)) * U(2);
			return PSCI_E_SUCCESS;
		}
		udelay(1);
	}

	ERROR("MT6797 power: big PLL frequency meter timeout\n");
	return PSCI_E_INTERN_FAIL;
}

static int mt6797_big_pll_retry(void)
{
	uint32_t value;
	int rc;

	/* Park the clock at 26 MHz before repeating the vendor PLL sequence. */
	if (mt6797_big_sem_get() != PSCI_E_SUCCESS)
		return PSCI_E_INTERN_FAIL;
	value = mmio_read_32(MT6797_BIG_PLL_DIV_MUX) & ~U(0x3);
	mmio_write_32(MT6797_BIG_PLL_DIV_MUX, value);
	udelay(1);
	mt6797_big_sem_put();

	value = mmio_read_32(MT6797_BIG_ARMPLL_CON0) & ~BIT_32(0);
	mmio_write_32(MT6797_BIG_ARMPLL_CON0, value);
	udelay(100);

	value = mmio_read_32(SPM_MP2_CPUSYS_PWR_CON) & ~BIT_32(2);
	mmio_write_32(SPM_MP2_CPUSYS_PWR_CON, value);
	rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS, MT6797_BIG_SPMC_ACK,
				false);
	if (rc != PSCI_E_SUCCESS)
		return rc;

	value = mmio_read_32(SPM_CPU_EXT_BUCK_ISO) | BIT_32(1);
	mmio_write_32(SPM_CPU_EXT_BUCK_ISO, value);
	udelay(100);
	value = mmio_read_32(SPM_CPU_EXT_BUCK_ISO) & ~BIT_32(1);
	mmio_write_32(SPM_CPU_EXT_BUCK_ISO, value);
	udelay(240);

	return PSCI_E_SUCCESS;
}

static int mt6797_power_on_big_cluster_locked(void)
{
	uint32_t value;
	uint32_t rate_khz;
	int rc;

	if (big_cluster_on)
		return PSCI_E_SUCCESS;

	/* External rails were enabled once at Linux handoff. Runtime CPU_ON
	 * never touches I2C6, the voltage selectors or the regulator enable bits.
	 */
	value = mmio_read_32(SPM_MP2_CPUSYS_PWR_CON) | BIT_32(0);
	mmio_write_32(SPM_MP2_CPUSYS_PWR_CON, value);
	rc = mt6797_toprgu_update_locked(11U, true);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	value = mmio_read_32(SPM_CPU_EXT_BUCK_ISO) & ~U(0x3);
	mmio_write_32(SPM_CPU_EXT_BUCK_ISO, value);
	rc = mt6797_toprgu_update_locked(11U, false);
	if (rc != PSCI_E_SUCCESS)
		return rc;

	for (unsigned int attempt = 0U;
	     attempt < MT6797_BIG_POWER_RETRIES; ++attempt) {
		value = mmio_read_32(SPM_MP2_CPUSYS_PWR_CON) | BIT_32(2);
		mmio_write_32(SPM_MP2_CPUSYS_PWR_CON, value);
		udelay(2);

		rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS,
				MT6797_BIG_SPMC_ACK, true);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		rc = mt6797_wait_mask(MT6797_BIG_CPUTOP_SPMC,
				MT6797_BIG_SPMC_ACK, true);
		if (rc != PSCI_E_SUCCESS)
			return rc;

		/* Vendor uses 0xa6800000 on the retry path to select the 500 MHz
		 * recovery PLL point. */
		value = mmio_read_32(SPM_MP2_CPUSYS_PWR_CON) & ~BIT_32(4);
		mmio_write_32(SPM_MP2_CPUSYS_PWR_CON, value);
		if (attempt != 0U)
			mmio_write_32(MT6797_BIG_ARMPLL_CON1, U(0xa6800000));
		udelay(20);

		mmio_write_32(MT6797_BIG_ARMPLL_CON0, U(0x00ff0100));
		udelay(1);
		mmio_write_32(MT6797_BIG_ARMPLL_CON0, U(0x00ff0101));
		udelay(10);
		mmio_write_32(MT6797_BIG_ARMPLL_CON0, U(0x00ff1101));
		udelay(30);

		if (mt6797_big_sem_get() != PSCI_E_SUCCESS)
			return PSCI_E_INTERN_FAIL;
		value = mmio_read_32(MT6797_BIG_PLL_DIV_MUX) | BIT_32(0);
		mmio_write_32(MT6797_BIG_PLL_DIV_MUX, value);
		udelay(1);
		value = (mmio_read_32(MT6797_BIG_CLKDIV) & U(0xffffffe0)) |
			U(0x8);
		mmio_write_32(MT6797_BIG_CLKDIV, value);
		udelay(1);
		mt6797_big_sem_put();

		rc = mt6797_big_measure_rate(&rate_khz);
		if (rc != PSCI_E_SUCCESS)
			return rc;

		if (((attempt == 0U) &&
		     (rate_khz > U(757500) || rate_khz < U(742500))) ||
		    ((attempt != 0U) &&
		     (rate_khz > U(505000) || rate_khz < U(495000)))) {
			INFO("MT6797 power: big PLL %u kHz, retry %u\n",
				rate_khz, attempt + 1U);
			if (attempt + 1U >= MT6797_BIG_POWER_RETRIES)
				return PSCI_E_INTERN_FAIL;
			rc = mt6797_big_pll_retry();
			if (rc != PSCI_E_SUCCESS)
				return rc;
			continue;
		}

		if (mt6797_big_sem_get() != PSCI_E_SUCCESS)
			return PSCI_E_INTERN_FAIL;
		mmio_write_32(MT6797_BIG_ARMPLL_K1, U(0xffffffff));
		mmio_write_32(MT6797_BIG_ARM_DIV_MON_EN, U(0));
		udelay(1);
		mt6797_big_sem_put();
		break;
	}

	value = mmio_read_32(MT6797_INFRA_PROTECTEN1) &
		~(BIT_32(2) | BIT_32(6) | BIT_32(10));
	mmio_write_32(MT6797_INFRA_PROTECTEN1, value);
	dsb();
	rc = mt6797_wait_mask(MT6797_INFRA_PROTECTSTA3,
			BIT_32(2) | BIT_32(6) | BIT_32(10), false);
	if (rc != PSCI_E_SUCCESS)
		return rc;

	value = mmio_read_32(MT6797_BIG_MISCDBG) & ~BIT_32(0);
	mmio_write_32(MT6797_BIG_MISCDBG, value);
	value = mmio_read_32(MP2_SNOOP_CTRL) | MT6797_CCI_SNOOP_ENABLE |
		((plat_my_core_pos() + 1U) << 4);
	mmio_write_32(MP2_SNOOP_CTRL, value);
	dsb();
	rc = mt6797_wait_mask(MP2_SNOOP_STATUS, BIT_32(0), false);
	if (rc != PSCI_E_SUCCESS)
		return rc;

	value = mmio_read_32(MT6797_BIG_SHAPER) | BIT_32(16);
	mmio_write_32(MT6797_BIG_SHAPER, value);
	big_cluster_on = true;
	INFO("MT6797 power: big cluster on at %u kHz\n", rate_khz);
	return PSCI_E_SUCCESS;
}

bool mt6797_big_cluster_has_core(void)
{
	return big_on != 0U;
}

int mt6797_power_on_big_core(unsigned int linear_id)
{
	uintptr_t power_control;
	uintptr_t spmc_status;
	uint32_t value;
	int rc = PSCI_E_SUCCESS;

	if (linear_id < 8U || linear_id > 9U)
		return PSCI_E_INVALID_PARAMS;

	if ((big_on & BIT_32(linear_id - 8U)) != 0U)
		goto out;

	rc = mt6797_power_on_big_cluster_locked();
	if (rc != PSCI_E_SUCCESS)
		goto out;

	power_control = SPM_MP2_CPU0_PWR_CON +
		((linear_id - 8U) * sizeof(uint32_t));
	spmc_status = (linear_id == 8U) ? MT6797_BIG_CPU0_SPMC :
		MT6797_BIG_CPU1_SPMC;
	if ((mmio_read_32(power_control) & BIT_32(2)) != 0U) {
		big_on |= BIT_32(linear_id - 8U);
		goto out;
	}

	/* This bit selects the AArch64 big-core reset path in Vendor LK. */
	mmio_write_32(CPUCFG, U(0xf) << 16);
	mmio_write_32(MP2_MISC_CONFIG_BOOT_ADDR_L(linear_id - 8U),
			(uint32_t)(uintptr_t)bl31_warm_entrypoint);
	mmio_write_32(MP2_MISC_CONFIG_BOOT_ADDR_H(linear_id - 8U), 0U);

	value = mmio_read_32(power_control) & ~BIT_32(0);
	mmio_write_32(power_control, value);
	value |= BIT_32(2);
	mmio_write_32(power_control, value);
	udelay(2);

	rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS,
		BIT_32(7U - (linear_id - 8U)), true);
	if (rc != PSCI_E_SUCCESS)
		goto out;
	rc = mt6797_wait_mask(spmc_status, MT6797_BIG_SPMC_ACK, true);
	if (rc != PSCI_E_SUCCESS)
		goto out;

	value = mmio_read_32(power_control) | BIT_32(0);
	mmio_write_32(power_control, value);
	dsb();

	/* Match Vendor big_spark2_setldo(0, 0) and big_spark2_core(). */
	mmio_write_32(MT6797_BIG_SPARK2LDO, U(0x3f));
	value = mmio_read_32(spmc_status) | BIT_32(0);
	mmio_write_32(spmc_status, value);
	big_on |= BIT_32(linear_id - 8U);

out:
	return rc;
}

void mt6797_power_off_prepare(unsigned int linear_id)
{
	mmio_write_32((uintptr_t)&off_pending[linear_id], 1U);
	dsb();
}

static int mt6797_power_off_little_cluster(unsigned int cluster)
{
	static const unsigned int bits[] = { 1U, 5U, 6U, 8U, 0U, 4U, 2U, 3U };
	static const bool values[] = { true, true, false, true, false, true,
		false, false };
	uintptr_t control = SPM_BASE + 0x210U + cluster * 4U;
	uintptr_t snoop = cluster == 0U ? MT6797_CCI_SI4_SNOOP_CTRL :
		MT6797_CCI_SI3_SNOOP_CTRL;
	uintptr_t axi = cluster == 0U ? MT6797_MP0_AXI_CONFIG :
		MT6797_MP1_AXI_CONFIG;
	uint32_t mask = cluster == 0U ?
		(BIT_32(0) | BIT_32(4) | BIT_32(8) | BIT_32(11)) :
		(BIT_32(1) | BIT_32(5) | BIT_32(9));
	int rc;

	mmio_write_32(snoop, mmio_read_32(snoop) & ~U(3));
	rc = mt6797_wait_mask(MT6797_CCI_STATUS, BIT_32(0), false);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	mmio_write_32(axi, mmio_read_32(axi) | BIT_32(4));
	rc = mt6797_wait_mask(SPM_CPU_IDLE_STA, BIT_32(20U + cluster), true);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	mmio_write_32(MT6797_INFRA_PROTECTEN1,
		mmio_read_32(MT6797_INFRA_PROTECTEN1) | mask);
	dsb();
	rc = mt6797_wait_mask(MT6797_INFRA_PROTECTSTA3, mask, true);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	for (unsigned int i = 0U; i < ARRAY_SIZE(bits); ++i) {
		mt6797_spm_set_bit(control, bits[i], values[i]);
		if (i == 3U) {
			rc = mt6797_wait_mask(control, BIT_32(12), true);
			if (rc != PSCI_E_SUCCESS)
				return rc;
		}
	}
	return mt6797_wait_mask(control, BIT_32(2) | BIT_32(3), false);
}

static int mt6797_power_off_little_core(unsigned int cpu)
{
	static const unsigned int bits[] = { 1U, 6U, 5U, 0U, 4U, 2U, 8U, 9U };
	static const bool values[] = { true, false, true, false, true, false,
		true, true };
	unsigned int cluster = cpu / 4U;
	uintptr_t misc = cluster == 0U ? MT6797_MP0_CA7L_MISC_CONFIG :
		MT6797_MP1_CA7L_MISC_CONFIG;
	uintptr_t spark = cluster == 0U ? MT6797_MP0_SPARKEN : MT6797_MP1_SPARKEN;
	uintptr_t control = SPM_BASE + 0x220U + cpu * 4U;
	uint32_t cluster_mask = cluster == 0U ? U(0xf) : U(0xf0);
	int rc;

	if ((little_on & BIT_32(cpu)) != 0U) {
		rc = mt6797_wait_mask(misc, BIT_32(24U + cpu % 4U), true);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		mmio_write_32(spark, mmio_read_32(spark) & ~BIT_32(cpu % 4U));
		rc = mt6797_wait_mask(SPM_CPU_RET_STATUS, BIT_32(cpu), false);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		udelay(1);
		for (unsigned int i = 0U; i < ARRAY_SIZE(bits); ++i) {
			if (i == 5U) {
				mmio_write_32(control, mmio_read_32(control) &
					~(BIT_32(2) | BIT_32(3) | BIT_32(16)));
				udelay(2);
			} else {
				mt6797_spm_set_bit(control, bits[i], values[i]);
			}
			if (i == 6U) {
				rc = mt6797_wait_mask(control, BIT_32(12), true);
				if (rc != PSCI_E_SUCCESS)
					return rc;
			}
		}
		rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS, BIT_32(15U - cpu), false);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS_2ND,
			BIT_32(15U - cpu), false);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		little_on &= ~BIT_32(cpu);
	}
	if ((little_on & cluster_mask) == 0U)
		return mt6797_power_off_little_cluster(cluster);
	return PSCI_E_SUCCESS;
}

static int mt6797_power_off_big_cluster(void)
{
	uint32_t value, mask = BIT_32(2) | BIT_32(6) | BIT_32(10);
	int rc;

	value = (mmio_read_32(MP2_SNOOP_CTRL) & ~U(3)) |
		((plat_my_core_pos() + 1U) << 4);
	mmio_write_32(MP2_SNOOP_CTRL, value);
	rc = mt6797_wait_mask(MP2_SNOOP_STATUS, BIT_32(0), false);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	mmio_write_32(MT6797_BIG_MISCDBG,
		mmio_read_32(MT6797_BIG_MISCDBG) | BIT_32(0) | BIT_32(4));
	rc = mt6797_wait_mask(SPM_CPU_IDLE_STA2, BIT_32(20), true);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	mmio_write_32(MT6797_INFRA_PROTECTEN1,
		mmio_read_32(MT6797_INFRA_PROTECTEN1) | mask);
	dsb();
	rc = mt6797_wait_mask(MT6797_INFRA_PROTECTSTA3, mask, true);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	rc = mt6797_big_sem_get();
	if (rc != PSCI_E_SUCCESS)
		return rc;
	mmio_write_32(MT6797_BIG_PLL_DIV_MUX,
		mmio_read_32(MT6797_BIG_PLL_DIV_MUX) & ~BIT_32(0));
	udelay(1);
	mt6797_big_sem_put();
	mmio_write_32(MT6797_BIG_ARMPLL_CON0,
		mmio_read_32(MT6797_BIG_ARMPLL_CON0) & ~BIT_32(0));
	(void)mmio_read_32(MT6797_BIG_ARMPLL_CON0);
	mt6797_spm_set_bit(SPM_MP2_CPUSYS_PWR_CON, 4U, true);
	mt6797_spm_set_bit(SPM_MP2_CPUSYS_PWR_CON, 2U, false);
	rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS, BIT_32(17), false);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	mmio_write_32(SPM_CPU_EXT_BUCK_ISO,
		mmio_read_32(SPM_CPU_EXT_BUCK_ISO) | BIT_32(1));
	mt6797_spm_set_bit(SPM_MP2_CPUSYS_PWR_CON, 0U, false);
	big_cluster_on = false;
	return PSCI_E_SUCCESS;
}

static int mt6797_power_off_big_core(unsigned int cpu)
{
	uintptr_t control = SPM_MP2_CPU0_PWR_CON + (cpu - 8U) * 4U;
	int rc;

	if ((big_on & BIT_32(cpu - 8U)) != 0U) {
		rc = mt6797_wait_mask(SPM_CPU_IDLE_STA2, BIT_32(cpu + 2U), true);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		mt6797_spm_set_bit(control, 2U, false);
		rc = mt6797_wait_mask(SPM_CPU_PWR_STATUS, BIT_32(15U - cpu), false);
		if (rc != PSCI_E_SUCCESS)
			return rc;
		mt6797_spm_set_bit(control, 0U, false);
		big_on &= ~BIT_32(cpu - 8U);
	}
	if (big_on == 0U)
		return mt6797_power_off_big_cluster();
	return PSCI_E_SUCCESS;
}

static int mt6797_power_off_finish_locked(unsigned int cpu)
{
	int rc;

	if (mmio_read_32((uintptr_t)&off_pending[cpu]) == 0U)
		return PSCI_E_SUCCESS;
	if (cpu == plat_my_core_pos())
		return PSCI_E_DENIED;
	rc = cpu < 8U ? mt6797_power_off_little_core(cpu) :
		mt6797_power_off_big_core(cpu);
	if (rc == PSCI_E_SUCCESS) {
		mmio_write_32((uintptr_t)&off_pending[cpu], 0U);
		dsb();
		INFO("MT6797 power: CPU%u physically off (peer CPU%u)\n",
			cpu, plat_my_core_pos());
	}
	return rc;
}

int mt6797_power_off_finish(u_register_t mpidr)
{
	int cpu = plat_core_pos_by_mpidr(mpidr);
	int rc;

	if (cpu < 0)
		return PSCI_E_INVALID_PARAMS;
	spin_lock(&power_lock);
	rc = mt6797_power_off_finish_locked((unsigned int)cpu);
	spin_unlock(&power_lock);
	return rc;
}

int mt6797_power_on_core(unsigned int cpu)
{
	unsigned int cluster = cpu / 4U;
	int rc;

	if (cpu >= PLATFORM_CORE_COUNT)
		return PSCI_E_INVALID_PARAMS;
	spin_lock(&power_lock);
	/* CPU_ON may arrive without an intervening AFFINITY_INFO poll. */
	rc = mt6797_power_off_finish_locked(cpu);
	if (rc != PSCI_E_SUCCESS)
		goto done;
	if (cpu >= 8U) {
		rc = mt6797_power_on_big_core(cpu);
		goto done;
	}
	if (!mt6797_little_cluster_has_core(cluster)) {
		rc = mt6797_power_on_little_cluster(cluster);
		if (rc != PSCI_E_SUCCESS)
			goto done;
	}
	uintptr_t config = cluster == 0U ? MP0_MISC_CONFIG3 : MP1_MISC_CONFIG3;
	uintptr_t boot = cluster == 0U ? MP0_MISC_CONFIG_BOOT_ADDR(cpu) :
		MP1_MISC_CONFIG_BOOT_ADDR(cpu - 4U);

	mmio_write_32(config, mmio_read_32(config) | U(0xf000));
	mmio_write_32(boot, (uint32_t)(uintptr_t)bl31_warm_entrypoint);
	rc = mt6797_power_on_little_core(cpu);
done:
	spin_unlock(&power_lock);
	return rc;
}
