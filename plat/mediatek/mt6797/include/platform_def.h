/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PLATFORM_DEF_H
#define PLATFORM_DEF_H

#include <arch.h>

#define PLAT_PRIMARY_CPU			U(0)

/*******************************************************************************
 * Platform binary types and generic constants
 *******************************************************************************/
#define PLATFORM_LINKER_FORMAT		"elf64-littleaarch64"
#define PLATFORM_LINKER_ARCH		aarch64

#if defined(IMAGE_BL31)
#define PLATFORM_STACK_SIZE		0x800
#endif

#define FIRMWARE_WELCOME_STR		"Booting Trusted Firmware\n"

#define PLAT_MAX_PWR_LVL		U(2)
#define PLAT_MAX_RET_STATE		U(1)
#define PLAT_MAX_OFF_STATE		U(2)

#define PLATFORM_CACHE_LINE_SIZE	64
#define PLATFORM_SYSTEM_COUNT		U(1)
#define PLATFORM_CLUSTER_COUNT		U(3)
#define PLATFORM_CLUSTER0_CORE_COUNT	U(4)
#define PLATFORM_CLUSTER1_CORE_COUNT	U(4)
#define PLATFORM_CLUSTER2_CORE_COUNT	U(2)
#define PLATFORM_CORE_COUNT		(PLATFORM_CLUSTER0_CORE_COUNT + \
					 PLATFORM_CLUSTER1_CORE_COUNT + \
					 PLATFORM_CLUSTER2_CORE_COUNT)
#define PLATFORM_MAX_CPUS_PER_CLUSTER	U(4)
#define PLATFORM_NUM_AFFS		(PLATFORM_SYSTEM_COUNT + \
					 PLATFORM_CLUSTER_COUNT + \
					 PLATFORM_CORE_COUNT)

#define SOC_CHIP_ID			U(0x6797)

/*******************************************************************************
 * Trusted SRAM layout
 *
 * The vendor boot chain passes its arguments in the first 0x800 bytes of
 * TZRAM and loads BL31 at 0x100800.  Keep this layout until the LK/ATF handoff
 * has been replaced and validated on hardware.
 *******************************************************************************/
#define ATF_ARG_BASE			UL(0x00100000)
#define ATF_ARG_SIZE			UL(0x00000800)
#define TZRAM_BASE			UL(0x00100000)
#define TZRAM_SIZE			UL(0x00022000)
#define BL31_BASE			(TZRAM_BASE + ATF_ARG_SIZE)
#define BL31_LIMIT			(TZRAM2_BASE + TZRAM2_SIZE)
#define BL31_TZRAM_SIZE			(TZRAM_SIZE - ATF_ARG_SIZE)

#define TZRAM2_BASE			(TZRAM_BASE + TZRAM_SIZE)
#define TZRAM2_SIZE			UL(0x00008000)
#define TZRAM2_LIMIT			(TZRAM2_BASE + TZRAM2_SIZE)
#define MT6797_BL31_DATA_BASE		UL(0x00114000)
#define MT6797_BL31_XLAT_BASE		TZRAM2_BASE
#define MT6797_BL31_COHERENT_BASE	UL(0x00129000)
#define RAM_CONSOLE_BASE		UL(0x0012d000)
#define RAM_CONSOLE_SIZE		UL(0x00001000)

/*******************************************************************************
 * Address space and translation table limits
 *******************************************************************************/
#define PLAT_PHY_ADDR_SPACE_SIZE	(1ULL << 32)
#define PLAT_VIRT_ADDR_SPACE_SIZE	(1ULL << 32)
#define MAX_XLAT_TABLES			7
#define MAX_MMAP_REGIONS		16

#define CACHE_WRITEBACK_SHIFT		6
#define CACHE_WRITEBACK_GRANULE		(1 << CACHE_WRITEBACK_SHIFT)

/*******************************************************************************
 * MT6797 peripheral map
 *******************************************************************************/
#define IO_PHYS				UL(0x10000000)
#define INFRACFG_AO_BASE		(IO_PHYS + 0x00001000)
#define SPM_BASE			(IO_PHYS + 0x00006000)
#define RGU_BASE			(IO_PHYS + 0x00007000)
#define MCUCFG_BASE			(IO_PHYS + 0x00220000)
#define EMI_MPU_BASE			(IO_PHYS + 0x00200000)

/* Device regions used by the initial bring-up. */
#define MTK_DEV_RNG0_BASE		IO_PHYS
#define MTK_DEV_RNG0_SIZE		UL(0x02000000)
#define MTK_DEV_RNG1_BASE		UL(0x19000000)
#define MTK_DEV_RNG1_SIZE		UL(0x00400000)

/*******************************************************************************
 * UART and system counter
 *******************************************************************************/
#define UART0_BASE			(IO_PHYS + 0x01002000)
#define UART_CLOCK			26000000
#define UART_BAUDRATE			921600
#define SYS_COUNTER_FREQ_IN_TICKS	13000000
#define SYS_COUNTER_FREQ_IN_MHZ		13

/*******************************************************************************
 * GICv3
 *******************************************************************************/
#define MT_GIC_BASE			UL(0x19000000)
#define MT_GIC_RDIST_BASE		UL(0x19200000)
#define MT_GIC_SIZE			UL(0x00400000)

/*******************************************************************************
 * CCI-400
 *******************************************************************************/
#define CCI400_BASE			UL(0x10390000)
#define CCI_SIZE			UL(0x00010000)
#define MT6797_CCI_SEC_ACCESS		(CCI400_BASE + 0x0008)
#define PLAT_MT_CCI_CLUSTER0_SL_IFACE_IX	4
#define PLAT_MT_CCI_CLUSTER1_SL_IFACE_IX	3
#define PLAT_MT_CCI_CLUSTER2_SL_IFACE_IX	5

/*******************************************************************************
 * CPU reset/mailbox registers
 *******************************************************************************/
#define MP0_MISC_CONFIG_BOOT_ADDR(cpu)	(MCUCFG_BASE + 0x0038 + ((cpu) * 8))
#define MP1_MISC_CONFIG_BOOT_ADDR(cpu)	(MCUCFG_BASE + 0x0238 + ((cpu) * 8))
#define MP2_MISC_CONFIG_BOOT_ADDR_L(cpu)	(MCUCFG_BASE + 0x2290 + ((cpu) * 8))
#define MP2_MISC_CONFIG_BOOT_ADDR_H(cpu)	(MCUCFG_BASE + 0x2294 + ((cpu) * 8))
#define MP0_MISC_CONFIG3			(MCUCFG_BASE + 0x003c)
#define MP1_MISC_CONFIG3			(MCUCFG_BASE + 0x023c)
#define MP2_MISC_CONFIG3			(MCUCFG_BASE + 0x2294)

#define CPUCFG				(MCUCFG_BASE + 0x2208)
#define CCI_CONFIG			(MCUCFG_BASE + 0x0654)
#define MP2_SNOOP_CTRL			UL(0x10396000)
#define MP2_SNOOP_STATUS		UL(0x1039000c)
#define MT6797_MP0_AXI_CONFIG		(MCUCFG_BASE + 0x002c)
#define MT6797_MP1_AXI_CONFIG		(MCUCFG_BASE + 0x022c)
#define MT6797_MP0_CA7L_MISC_CONFIG	(MCUCFG_BASE + 0x0064)
#define MT6797_MP1_CA7L_MISC_CONFIG	(MCUCFG_BASE + 0x0264)
#define MT6797_MP0_SPARKVRETCNTRL	(MCUCFG_BASE + 0x1c00)
#define MT6797_MP0_SPARKEN		(MCUCFG_BASE + 0x1c04)
#define MT6797_MP1_SPARKVRETCNTRL	(MCUCFG_BASE + 0x3c00)
#define MT6797_MP1_SPARKEN		(MCUCFG_BASE + 0x3c04)
#define MT6797_INFRA_PROTECTEN1		UL(0x10001234)
#define MT6797_INFRA_PROTECTSTA3		UL(0x1000123c)
#define MT6797_CCI_SI3_SNOOP_CTRL	(CCI400_BASE + 0x4000)
#define MT6797_CCI_SI4_SNOOP_CTRL	(CCI400_BASE + 0x5000)
#define MT6797_CCI_STATUS		(CCI400_BASE + 0x000c)
#define MT6797_BIG_PLL_DIV_MUX		UL(0x1001a270)
#define MT6797_BIG_CLKDIV		UL(0x1001a274)
#define MT6797_BIG_ARMPLL_K1		UL(0x1001a27c)
#define MT6797_BIG_ARM_DIV_MON_EN	UL(0x1001a284)
#define MT6797_BIG_HW_SEM_ENABLE	UL(0x11015000)
#define MT6797_BIG_HW_SEM		UL(0x11015448)
#define MT6797_BIG_CLK_DBG_CFG		UL(0x1000010c)
#define MT6797_BIG_CLK26CALI_0		UL(0x10000220)
#define MT6797_BIG_CLK26CALI_1		UL(0x10000224)
#define MT6797_BIG_CLK_MISC_CFG_0	UL(0x10000104)
#define MT6797_BIG_MISCDBG		(MCUCFG_BASE + 0x220c)
#define MT6797_BIG_SHAPER		(MCUCFG_BASE + 0x2590)
#define MT6797_BIG_ARMPLL_CON0		(MCUCFG_BASE + 0x24a0)
#define MT6797_BIG_ARMPLL_CON1		(MCUCFG_BASE + 0x24a4)
#define MT6797_BIG_CPUTOP_SPMC		(MCUCFG_BASE + 0x22a0)
#define MT6797_BIG_CPU0_SPMC		(MCUCFG_BASE + 0x2430)
#define MT6797_BIG_CPU1_SPMC		(MCUCFG_BASE + 0x2434)
#define MT6797_BIG_SPARK2LDO		(MCUCFG_BASE + 0x2700)

/*******************************************************************************
 * SPM power-controller registers
 *******************************************************************************/
#define SPM_CPU_PWR_STATUS		(SPM_BASE + 0x0188)
#define SPM_CPU_IDLE_STA			(SPM_BASE + 0x0174)
#define SPM_CPU_IDLE_STA2			(SPM_BASE + 0x0178)
#define SPM_CPU_PWR_STATUS_2ND		(SPM_BASE + 0x018c)
#define SPM_CPU_RET_STATUS		(SPM_BASE + 0x01e4)
#define SPM_MP0_CPUSYS_PWR_CON		(SPM_BASE + 0x0210)
#define SPM_MP1_CPUSYS_PWR_CON		(SPM_BASE + 0x0214)
#define SPM_MP2_CPUSYS_PWR_CON		(SPM_BASE + 0x0218)
#define SPM_MP2_CPU0_PWR_CON		(SPM_BASE + 0x0240)
#define SPM_CPU_EXT_BUCK_ISO		(SPM_BASE + 0x0290)

/*******************************************************************************
 * Big-cluster reset controller
 *******************************************************************************/
#define MT6797_WDT_SWSYSRST		(RGU_BASE + 0x0018)

/*******************************************************************************
 * Watchdog reset
 *******************************************************************************/
#define MTK_WDT_BASE			(RGU_BASE)
#define MTK_WDT_MODE			(MTK_WDT_BASE + 0x0000)
#define MTK_WDT_SWRST			(MTK_WDT_BASE + 0x0014)
#define MTK_WDT_MODE_KEY			UL(0x22000000)
#define MTK_WDT_MODE_EXTEN		UL(0x00000004)
#define MTK_WDT_SWRST_KEY		UL(0x00001209)

#define NR_INT_POL_CTL			19
#define SEC_POL_CTL_EN0			(MCUCFG_BASE + 0x0a00)

#endif /* PLATFORM_DEF_H */
