/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <assert.h>
#include <bl31/bl31.h>
#include <common/debug.h>
#include <drivers/console.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <plat/common/platform.h>

#include <plat_private.h>
#include <platform_def.h>

DEFINE_RENAME_SYSREG_RW_FUNCS(mt6797_cpuectlr_el1, S3_1_C15_C2_1)
DEFINE_RENAME_SYSREG_RW_FUNCS(mt6797_l2actlr_el1, S3_1_C15_C0_0)
DEFINE_RENAME_SYSREG_RW_FUNCS(mt6797_l2ectlr_el1, S3_1_C11_C0_3)

/* BSP plat_affinst_suspend()/finish() context, retained in trusted SRAM. */
static struct {
	u_register_t l2actlr, l2ectlr;
	uint32_t cache_config, axi, spark;
	bool valid;
} suspend_context;

static void mt6797_cpu_standby(plat_local_state_t cpu_state)
{
	u_register_t scr = read_scr_el3();

	(void)cpu_state;
	/* WFI at EL3 must see the non-secure wake interrupt, even with DAIF
	 * masked. Restore routing before returning to the caller's context.
	 */
	write_scr_el3(scr | SCR_IRQ_BIT | SCR_FIQ_BIT);
	isb();
	dsbsy();
	wfi();
	write_scr_el3(scr);
	isb();
}

static int mt6797_power_domain_on(u_register_t mpidr)
{
	int linear_id = plat_core_pos_by_mpidr(mpidr);

	if (linear_id < 0)
		return PSCI_E_INVALID_PARAMS;
	return mt6797_power_on_core((unsigned int)linear_id);
}

static void mt6797_power_domain_off(const psci_power_state_t *target_state)
{
	unsigned int cpu = plat_my_core_pos();

	mt_gic_cpuif_disable();
	/* BSP SPARK2 CPU-side retention control must be clear before WFI. */
	write_mt6797_cpuectlr_el1(read_mt6797_cpuectlr_el1() & ~UL(7));
	isb();
	if ((cpu < 8U) &&
	    is_local_state_off(target_state->pwr_domain_state[1])) {
		uintptr_t axi = cpu < 4U ? MT6797_MP0_AXI_CONFIG :
			MT6797_MP1_AXI_CONFIG;

		plat_mtk_cci_disable();
		mmio_write_32(axi, mmio_read_32(axi) | BIT_32(4));
	}
	mt6797_power_off_prepare(cpu);
}

static void mt6797_power_domain_on_finish(const psci_power_state_t *target_state)
{
	(void)target_state;
	plat_mtk_cci_enable();
	mt_gic_rdistif_init();
	mt_gic_cpuif_enable();
}

static void mt6797_power_domain_suspend(const psci_power_state_t *target_state)
{
	if (!is_local_state_off(target_state->pwr_domain_state[0]))
		return;
	assert(plat_my_core_pos() == 0U);
	assert(is_local_state_off(target_state->pwr_domain_state[1]));
	suspend_context.l2actlr = read_mt6797_l2actlr_el1();
	suspend_context.l2ectlr = read_mt6797_l2ectlr_el1();
	suspend_context.cache_config = mmio_read_32(MCUCFG_BASE);
	suspend_context.axi = mmio_read_32(MT6797_MP0_AXI_CONFIG);
	suspend_context.spark = mmio_read_32(MT6797_MP0_SPARKEN);
	suspend_context.valid = true;
	mmio_setbits_32(MP0_MISC_CONFIG3, BIT_32(12));
	mmio_write_32(MP0_MISC_CONFIG_BOOT_ADDR(0U),
		     (uint32_t)(uintptr_t)bl31_warm_entrypoint);
	write_mt6797_cpuectlr_el1(read_mt6797_cpuectlr_el1() & ~UL(7));
	isb();
	mmio_clrbits_32(MT6797_MP0_SPARKEN, BIT_32(0));
	mmio_clrbits_32(MCUCFG_BASE, BIT_32(4)); /* L2RSTDISABLE */
	mt6797_gic_suspend();
	plat_mtk_cci_disable();
	mmio_write_32(MT6797_MP0_AXI_CONFIG,
		     suspend_context.axi | BIT_32(4));
	dsbsy();
}

static void mt6797_power_domain_suspend_finish(const psci_power_state_t *target_state)
{
	if (!is_local_state_off(target_state->pwr_domain_state[0]))
		return;
	assert(plat_my_core_pos() == 0U && suspend_context.valid);
	write_mt6797_l2actlr_el1(suspend_context.l2actlr);
	write_mt6797_l2ectlr_el1(suspend_context.l2ectlr);
	isb();
	mmio_write_32(MCUCFG_BASE, suspend_context.cache_config);
	mmio_write_32(MT6797_MP0_AXI_CONFIG, suspend_context.axi);
	dsbsy();
	plat_mtk_cci_enable();
	mt6797_gic_resume();
	mmio_write_32(MT6797_MP0_SPARKEN, suspend_context.spark);
	mmio_write_32(MP0_MISC_CONFIG_BOOT_ADDR(0U), 0U);
	suspend_context.valid = false;
}

static __dead2 void mt6797_system_off(void)
{
	int ret;

	/* A failed PMIC shutdown must not turn into a watchdog reboot. */
	mmio_write_32(MTK_WDT_MODE, MTK_WDT_MODE_KEY | MTK_WDT_MODE_EXTEN);
	console_switch_state(CONSOLE_FLAG_BOOT);
	ret = mt6797_pmic_power_off();
	ERROR("MT6351 power-off failed: %d; system halted\n", ret);
	for (;;) {
		dsbsy();
		wfi();
	}
}

static __dead2 void mt6797_system_reset(void)
{
	mmio_write_32(MTK_WDT_MODE, MTK_WDT_MODE_KEY | MTK_WDT_MODE_EXTEN);
	mmio_write_32(MTK_WDT_SWRST, MTK_WDT_SWRST_KEY);
	panic();
}

static int mt6797_validate_power_state(unsigned int power_state,
					psci_power_state_t *req_state)
{
	/* State 1 is CPU0 + MP0 powerdown, only with an armed sleep PCM and
	 * every other physical CPU off. Never allow an unprepared idle caller
	 * or the memory-DVFS common PCM to initiate this sequence.
	 */
	if (psci_get_pstate_type(power_state) == PSTATE_TYPE_POWERDOWN &&
	    psci_get_pstate_id(power_state) == 1U &&
	    psci_get_pstate_pwrlvl(power_state) == 1U) {
		uint32_t flags = mmio_read_32(SPM_BASE + U(0x600));

		if (plat_my_core_pos() != 0U ||
		    (flags & (BIT_32(0) | BIT_32(10))) != 0U ||
		    (mmio_read_32(SPM_BASE + U(0x02c)) & U(0x81)) != U(0x81) ||
		    (mmio_read_32(SPM_CPU_PWR_STATUS) & U(0x7fc0)) != 0U)
			return PSCI_E_INVALID_PARAMS;
		req_state->pwr_domain_state[0] = PLAT_MAX_OFF_STATE;
		req_state->pwr_domain_state[1] = PLAT_MAX_OFF_STATE;
		return PSCI_E_SUCCESS;
	}
	/* State 0 retains the CPU and all context. */
	if (psci_get_pstate_type(power_state) != PSTATE_TYPE_STANDBY ||
	    psci_get_pstate_id(power_state) != 0U ||
	    psci_get_pstate_pwrlvl(power_state) != 0U)
		return PSCI_E_INVALID_PARAMS;
	req_state->pwr_domain_state[0] = PLAT_MAX_RET_STATE;
	return PSCI_E_SUCCESS;
}

static const plat_psci_ops_t mt6797_psci_ops = {
	.cpu_standby = mt6797_cpu_standby,
	.pwr_domain_on = mt6797_power_domain_on,
	.pwr_domain_off = mt6797_power_domain_off,
	.pwr_domain_off_finish = mt6797_power_off_finish,
	.pwr_domain_suspend = mt6797_power_domain_suspend,
	.pwr_domain_on_finish = mt6797_power_domain_on_finish,
	.pwr_domain_suspend_finish = mt6797_power_domain_suspend_finish,
	.system_off = mt6797_system_off,
	.system_reset = mt6797_system_reset,
	.validate_power_state = mt6797_validate_power_state,
};

int plat_setup_psci_ops(uintptr_t sec_entrypoint,
			const plat_psci_ops_t **psci_ops)
{
	(void)sec_entrypoint;
	*psci_ops = &mt6797_psci_ops;
	return 0;
}
