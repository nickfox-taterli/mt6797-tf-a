/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <drivers/console.h>
#include <drivers/generic_delay_timer.h>
#include <drivers/ti/uart/uart_16550.h>
#include <lib/mmio.h>
#include <lib/utils_def.h>

#include <plat_private.h>
#include <platform_def.h>
#include <mtk_plat_common.h>

#if defined(MACH_MT6797)
extern void mt6797_early_uart_putc(unsigned int c);
#define MT6797_SETUP_MARKER(_c) mt6797_early_uart_putc(_c)
#else
#define MT6797_SETUP_MARKER(_c) do { } while (0)
#endif

static entry_point_info_t bl32_ep_info;
static entry_point_info_t bl33_ep_info;

static void mt6797_syscnt_setup(void)
{
	uintptr_t index = MCUCFG_BASE + U(0x674);
	uintptr_t control = MCUCFG_BASE + U(0x670);
	uint32_t before, after;

	/* BSP setup_syscnt(): CPUxGPT's 26 MHz input must be divided by two
	 * before using the 13 MHz CNTFRQ. Mainline has no vendor GPT driver
	 * to perform this initialization. Do not reset the running counter.
	 */
	mmio_write_32(index, U(0));
	before = mmio_read_32(control);
	after = (before & ~U(0x700)) | U(0x201);
	mmio_write_32(control, after);
	dsbsy();
	INFO("MT6797 CPUxGPT: control 0x%x -> 0x%x (13 MHz)\n",
	      before, mmio_read_32(control));
}

entry_point_info_t *bl31_plat_get_next_image_ep_info(uint32_t type)
{
	entry_point_info_t *next_image_info;

	assert(sec_state_is_valid(type));
	next_image_info = (type == NON_SECURE) ? &bl33_ep_info : &bl32_ep_info;
	assert(next_image_info->h.type == PARAM_EP);

	return next_image_info->pc != 0U ? next_image_info : NULL;
}

static uint32_t mt6797_bl33_spsr(void)
{
#ifdef X20_BL33_AARCH64
	return SPSR_64(MODE_EL2, MODE_SP_ELX, DISABLE_ALL_EXCEPTIONS);
#else
	return SPSR_MODE32(MODE32_svc, SPSR_T_ARM, SPSR_E_LITTLE,
			   DAIF_FIQ_BIT | DAIF_IRQ_BIT | DAIF_ABT_BIT);
#endif
}

entry_point_info_t *bl31_plat_get_next_kernel64_ep_info(uint32_t type)
{
	unsigned int mode;

	assert(sec_state_is_valid(type));
	assert(type == NON_SECURE);

	mode = (el_implemented(2) != EL_IMPL_NONE) ? MODE_EL2 : MODE_EL1;
	bl33_ep_info.spsr = SPSR_64(mode, MODE_SP_ELX,
				    DISABLE_ALL_EXCEPTIONS);
	bl33_ep_info.pc = get_kernel_info_pc();
	bl33_ep_info.args.arg0 = get_kernel_info_r0();
	bl33_ep_info.args.arg1 = get_kernel_info_r1();
	bl33_ep_info.args.arg2 = 0U;
	bl33_ep_info.args.arg3 = 0U;
	SET_SECURITY_STATE(bl33_ep_info.h.attr, NON_SECURE);

	return bl33_ep_info.pc != 0U ? &bl33_ep_info : NULL;
}

entry_point_info_t *bl31_plat_get_next_kernel32_ep_info(uint32_t type)
{
	assert(sec_state_is_valid(type));
	assert(type == NON_SECURE);

	bl33_ep_info.spsr = SPSR_MODE32(MODE32_hyp, SPSR_T_ARM,
					SPSR_E_LITTLE,
					DAIF_FIQ_BIT | DAIF_IRQ_BIT | DAIF_ABT_BIT);
	bl33_ep_info.pc = get_kernel_info_pc();
	bl33_ep_info.args.arg0 = get_kernel_info_r0();
	bl33_ep_info.args.arg1 = get_kernel_info_r1();
	bl33_ep_info.args.arg2 = get_kernel_info_r2();
	bl33_ep_info.args.arg3 = 0U;
	SET_SECURITY_STATE(bl33_ep_info.h.attr, NON_SECURE);

	return bl33_ep_info.pc != 0U ? &bl33_ep_info : NULL;
}

void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)
{
	static console_t console;

	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;

	MT6797_SETUP_MARKER('E');
	/* The preloader already configured this MTK UART; do not retune it. */
	console_16550_register(UART0_BASE, 0U, 0U, &console);
#ifdef X20_WDT_FIQ
	console_set_scope(&console, CONSOLE_FLAG_BOOT | CONSOLE_FLAG_RUNTIME |
			  CONSOLE_FLAG_CRASH);
#endif
	MT6797_SETUP_MARKER('F');
	mt6797_syscnt_setup();

	SET_PARAM_HEAD(&bl32_ep_info, PARAM_EP, VERSION_1, 0);
	SET_SECURITY_STATE(bl32_ep_info.h.attr, SECURE);

	SET_PARAM_HEAD(&bl33_ep_info, PARAM_EP, VERSION_1, 0);
	SET_SECURITY_STATE(bl33_ep_info.h.attr, NON_SECURE);
	bl33_ep_info.pc = (uintptr_t)mt6797_boot_bl33_entrypoint;
	bl33_ep_info.spsr = mt6797_bl33_spsr();
#ifndef X20_BL33_AARCH64
	bl33_ep_info.args.arg4 = mt6797_boot_argument_location;
	bl33_ep_info.args.arg5 = mt6797_boot_argument_size;
#endif

#ifdef X20_BL33_AARCH64
	NOTICE("MT6797 BL31: U-Boot(AArch64)=0x%llx\n",
	       (unsigned long long)bl33_ep_info.pc);
#else
	NOTICE("MT6797 BL31: LK=0x%llx args=0x%llx/0x%llx\n",
		(unsigned long long)bl33_ep_info.pc,
		(unsigned long long)bl33_ep_info.args.arg4,
		(unsigned long long)bl33_ep_info.args.arg5);
#endif
}

void bl31_platform_setup(void)
{
	generic_delay_timer_init();
	mt_gic_driver_init();
	mt_gic_init();
#ifdef X20_BL33_AARCH64
	/* U-Boot boots Linux directly from EL2 rather than using LK's kernel
	 * entry SMC. Finish the same one-time rail bootstrap before BL33. */
	if (mt6797_kernel_power_prepare() != 0)
		panic();
#endif
#ifdef X20_WDT_FIQ
	mt6797_wdt_capture_init();
#endif
}

void bl31_plat_arch_setup(void)
{
	plat_mtk_cci_init();
	plat_mtk_cci_enable();
	/* Match Vendor BL31: allow the non-secure kernel to access CCI-400. */
	mmio_write_32(MT6797_CCI_SEC_ACCESS, U(0x1));
	plat_configure_mmu_el3(BL_CODE_BASE,
				       BL_COHERENT_RAM_END - BL_CODE_BASE,
				       BL_CODE_BASE,
				       BL_CODE_END,
				       BL_COHERENT_RAM_BASE,
				       BL_COHERENT_RAM_END);
}
