#
# Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause
#

MTK_PLAT	 := plat/mediatek
MTK_PLAT_SOC	 := ${MTK_PLAT}/${PLAT}

PLAT_INCLUDES := -I${MTK_PLAT}/common/ \
		 -I${MTK_PLAT}/drivers/uart/ \
		 -I${MTK_PLAT}/include/ \
		 -I${MTK_PLAT_SOC}/include/

include drivers/arm/gic/v3/gicv3.mk

# Production X20 builds require this. The WDT IRQ stays non-secure until
# Linux explicitly arms secure FIQ capture; the FIQ handler never takes locks.
ifeq (${X20_WDT_FIQ},1)
$(eval $(call add_define,X20_WDT_FIQ))
BL31_SOURCES += ${MTK_PLAT_SOC}/wdt_capture.c
endif

# U-Boot proper is AArch64; retain the established AArch32 LK ABI by default.
ifeq (${X20_BL33_AARCH64},1)
$(eval $(call add_define,X20_BL33_AARCH64))
endif

# Bring-up only: single-character progress markers on the raw UART before the
# C console exists. Off by default; they are pure boot noise otherwise.
ifeq (${X20_EARLY_MARKERS},1)
$(eval $(call add_define,X20_EARLY_MARKERS))
endif

PLAT_BL_COMMON_SOURCES := \
	lib/xlat_tables/aarch64/xlat_tables.c \
	lib/xlat_tables/xlat_tables_common.c \
	plat/common/plat_psci_common.c \
	plat/common/aarch64/crash_console_helpers.S

	BL31_SOURCES += \
	common/desc_image_load.c \
	drivers/arm/cci400/cci400.c \
	${GICV3_SOURCES} \
	drivers/delay_timer/delay_timer.c \
	drivers/delay_timer/generic_delay_timer.c \
	drivers/ti/uart/aarch64/16550_console.S \
	lib/bl_aux_params/bl_aux_params.c \
	lib/cpus/aarch64/aem_generic.S \
	lib/cpus/aarch64/cortex_a53.S \
	lib/cpus/aarch64/cortex_a72.S \
	plat/common/plat_gicv3.c \
	${MTK_PLAT}/common/mtk_plat_common.c \
	${MTK_PLAT_SOC}/aarch64/plat_helpers.S \
	${MTK_PLAT_SOC}/aarch64/platform_common.c \
	${MTK_PLAT_SOC}/bl31_plat_setup.c \
	${MTK_PLAT_SOC}/boot_power.c \
	${MTK_PLAT_SOC}/idvfs.c \
	${MTK_PLAT_SOC}/plat_mt_gic.c \
	${MTK_PLAT_SOC}/plat_pm.c \
	${MTK_PLAT_SOC}/pmic.c \
	${MTK_PLAT_SOC}/power.c \
	${MTK_PLAT_SOC}/mt6797_emi_mpu.c \
	${MTK_PLAT_SOC}/plat_sip_calls.c \
	${MTK_PLAT_SOC}/plat_topology.c

ERRATA_A53_826319 := 0
ERRATA_A53_835769 := 1
ERRATA_A53_836870 := 1
ERRATA_A53_843419 := 1
ERRATA_A53_855873 := 1
# X20: A53 r0p4 and A72 r0p1. Both need speculative-AT context handling;
# the A72 also needs the reset-time instruction-prefetch workaround.
ERRATA_A53_1530924 := 1
ERRATA_A72_859971 := 1
ERRATA_A72_1319367 := 1

RESET_TO_BL31 := 1
PROGRAMMABLE_RESET_ADDRESS := 1
COLD_BOOT_SINGLE_CPU := 1

MACH_MT6797 := 1
$(eval $(call add_define,MACH_MT6797))

include lib/coreboot/coreboot.mk
