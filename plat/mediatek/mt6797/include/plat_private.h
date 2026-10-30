/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PLAT_PRIVATE_H
#define PLAT_PRIVATE_H

#include <stdint.h>
#include <stdbool.h>

#include <common/bl_common.h>

void plat_configure_mmu_el3(uintptr_t total_base,
				    uintptr_t total_size,
				    uintptr_t ro_start,
				    uintptr_t ro_limit,
				    uintptr_t coh_start,
				    uintptr_t coh_limit);

void plat_mtk_cci_init(void);
void plat_mtk_cci_enable(void);
void plat_mtk_cci_disable(void);

void mt_gic_driver_init(void);
void mt_gic_init(void);
void mt_gic_cpuif_enable(void);
void mt_gic_cpuif_disable(void);
void mt_gic_rdistif_init(void);
void mt6797_gic_suspend(void);
void mt6797_gic_resume(void);
#ifdef X20_WDT_FIQ
void mt6797_wdt_capture_init(void);
int mt6797_wdt_capture_control(u_register_t command, u_register_t mask);
#endif

int mt6797_toprgu_update(unsigned int id, bool assert);
int mt6797_boot_buck_enable(void);
int mt6797_kernel_power_prepare(void);
bool mt6797_kernel_power_ready(void);
int mt6797_idvfs_set_big_sram_ldo(unsigned int mv_x100);
int mt6797_power_on_little_cluster(unsigned int cluster_id);
int mt6797_power_on_little_core(unsigned int linear_id);
bool mt6797_little_cluster_has_core(unsigned int cluster_id);
int mt6797_power_on_big_core(unsigned int linear_id);
bool mt6797_big_cluster_has_core(void);
int mt6797_power_on_core(unsigned int linear_id);
void mt6797_power_off_prepare(unsigned int linear_id);
int mt6797_power_off_finish(u_register_t mpidr);
int mt6797_pmic_power_off(void);

/* Values captured before el3_entrypoint_common changes the boot registers. */
extern uint64_t mt6797_boot_argument_location;
extern uint64_t mt6797_boot_argument_size;
extern uint64_t mt6797_boot_bl33_entrypoint;
extern uint64_t mt6797_boot_tee_argument;

#endif /* PLAT_PRIVATE_H */
