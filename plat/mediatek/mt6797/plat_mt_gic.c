/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <drivers/arm/gicv3.h>
#include <plat/common/platform.h>
#include <platform_def.h>

#include <plat_private.h>

#ifdef X20_WDT_FIQ
static const interrupt_prop_t capture_interrupts[] = {
	INTR_PROP_DESC(15, 0, INTR_GROUP0, GIC_INTR_CFG_EDGE),
};
#endif

static uintptr_t rdistif_base_addrs[PLATFORM_CORE_COUNT];
static gicv3_dist_ctx_t suspend_dist;
static gicv3_redist_ctx_t suspend_redist;

static unsigned int mt_mpidr_to_core_pos(u_register_t mpidr)
{
	return plat_core_pos_by_mpidr(mpidr);
}

static const gicv3_driver_data_t mt6797_gic_data = {
	.gicd_base = MT_GIC_BASE,
	.gicr_base = MT_GIC_RDIST_BASE,
	.rdistif_num = PLATFORM_CORE_COUNT,
	.rdistif_base_addrs = rdistif_base_addrs,
	.mpidr_to_core_pos = mt_mpidr_to_core_pos,
#ifdef X20_WDT_FIQ
	.interrupt_props = capture_interrupts,
	.interrupt_props_num = ARRAY_SIZE(capture_interrupts),
#endif
};

void mt_gic_driver_init(void)
{
	gicv3_driver_init(&mt6797_gic_data);
}

void mt_gic_init(void)
{
	unsigned int core_pos = plat_my_core_pos();

	gicv3_distif_init();
	gicv3_rdistif_init(core_pos);
	gicv3_cpuif_enable(core_pos);
}

void mt_gic_cpuif_enable(void)
{
	gicv3_cpuif_enable(plat_my_core_pos());
}

void mt_gic_cpuif_disable(void)
{
	gicv3_cpuif_disable(plat_my_core_pos());
}

void mt_gic_rdistif_init(void)
{
	gicv3_rdistif_init(plat_my_core_pos());
}

/* SPM system sleep is entered by CPU0 after all other CPUs are offline. */
void mt6797_gic_suspend(void)
{
	gicv3_cpuif_disable(0U);
	gicv3_rdistif_save(0U, &suspend_redist);
	gicv3_distif_save(&suspend_dist);
}

void mt6797_gic_resume(void)
{
	gicv3_distif_init_restore(&suspend_dist);
	gicv3_rdistif_init_restore(0U, &suspend_redist);
	gicv3_cpuif_enable(0U);
}
