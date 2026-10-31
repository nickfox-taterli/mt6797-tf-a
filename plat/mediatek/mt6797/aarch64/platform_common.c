/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <lib/mmio.h>
#include <lib/utils.h>
#include <lib/xlat_tables/xlat_tables.h>

#include <plat_private.h>
#include <platform_def.h>
#include <mt6797_cci400.h>

#if defined(X20_EARLY_MARKERS)
extern void mt6797_early_uart_putc(unsigned int c);
#define MT6797_MMU_MARKER(c) mt6797_early_uart_putc(c)
#else
#define MT6797_MMU_MARKER(c) do { } while (0)
#endif

const mmap_region_t plat_mmap[] = {
	MAP_REGION_FLAT(TZRAM_BASE, TZRAM_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(TZRAM2_BASE, TZRAM2_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(MTK_DEV_RNG0_BASE, MTK_DEV_RNG0_SIZE,
			MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(MTK_DEV_RNG1_BASE, MTK_DEV_RNG1_SIZE,
			MT_DEVICE | MT_RW | MT_SECURE),
	MAP_REGION_FLAT(RAM_CONSOLE_BASE, RAM_CONSOLE_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE),
	{ 0 }
};

void plat_configure_mmu_el3(uintptr_t total_base,
				    uintptr_t total_size,
				    uintptr_t ro_start,
				    uintptr_t ro_limit,
				    uintptr_t coh_start,
				    uintptr_t coh_limit)
{
	uintptr_t total_region_base = round_down(total_base, PAGE_SIZE);
	uintptr_t total_region_limit = round_up(total_base + total_size,
							PAGE_SIZE);
	uintptr_t ro_region_base = round_down(ro_start, PAGE_SIZE);
	uintptr_t ro_region_limit = round_up(ro_limit, PAGE_SIZE);
	uintptr_t coh_region_base = round_down(coh_start, PAGE_SIZE);
	uintptr_t coh_region_limit = round_up(coh_limit, PAGE_SIZE);

	MT6797_MMU_MARKER('G');
	mmap_add_region(total_region_base, total_region_base,
			total_region_limit - total_region_base,
			MT_MEMORY | MT_RW | MT_SECURE);
	MT6797_MMU_MARKER('H');
	mmap_add_region(ro_region_base, ro_region_base,
			ro_region_limit - ro_region_base,
			MT_MEMORY | MT_RO | MT_SECURE);
	MT6797_MMU_MARKER('I');
	mmap_add_region(coh_region_base, coh_region_base,
			coh_region_limit - coh_region_base,
			MT_DEVICE | MT_RW | MT_SECURE);
	MT6797_MMU_MARKER('J');
	mmap_add(plat_mmap);
	MT6797_MMU_MARKER('K');
	init_xlat_tables();
	MT6797_MMU_MARKER('L');
	enable_mmu_el3(0);
	MT6797_MMU_MARKER('M');
}

unsigned int plat_get_syscnt_freq2(void)
{
	return SYS_COUNTER_FREQ_IN_TICKS;
}

void plat_mtk_cci_init(void)
{
	mcsi_a_init(CCI400_BASE,
		PLAT_MT_CCI_CLUSTER0_SL_IFACE_IX,
		PLAT_MT_CCI_CLUSTER1_SL_IFACE_IX,
		PLAT_MT_CCI_CLUSTER2_SL_IFACE_IX);
}

void plat_mtk_cci_enable(void)
{
	unsigned int cluster = MPIDR_AFFLVL1_VAL(read_mpidr());

	/* The PSCI cluster lifecycle owns ACINACTM and CCI coherency. */
	if (cluster < 2U) {
		uintptr_t axi = cluster == 0U ? MT6797_MP0_AXI_CONFIG :
			MT6797_MP1_AXI_CONFIG;

		mmio_clrbits_32(axi, BIT_32(4));
		dsbsy();
	}
	mcsi_a_enable_cluster_dcm(CCI400_BASE);
	cci_enable_cluster_coherency(read_mpidr());
}

void plat_mtk_cci_disable(void)
{
	cci_disable_cluster_coherency(read_mpidr());
}
