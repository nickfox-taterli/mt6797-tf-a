/*
 * Copyright (c) 2026, MediaTek Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * MT6797 early-entry platform hook for bl31_entrypoint.
 *
 * The AArch32 SPL/LK boot ABI hands BL31 four 32-bit values in r4..r7; a
 * warm reset to AArch64 leaves the upper halves of x4..x7 undefined, so
 * they are zero-extended before saving.  This runs before any generic or
 * platform C code can clobber them: no upstream hook receives these
 * registers, and later reads would depend on register preservation across
 * calls that is not a stable contract.  bl31_plat_setup consumes the saved
 * slots; they live in a dedicated rodata section (mt6797_boot_args.S)
 * because the generic entry code clears .data/.bss before bl31_main()
 * reads them.
 *
 * The macro must stay a single inline expansion: it runs with no stack and
 * may not clobber x0..x3, x20..x23, or the SP.
 */

#ifndef PLAT_EARLY_ENTRY_MACROS_H
#define PLAT_EARLY_ENTRY_MACROS_H

#ifdef __ASSEMBLER__

/* Tells the generic bl31_entrypoint that plat_early_boot_arg_capture below
 * is the platform-provided capture; without it the generic empty default
 * is used. */
#define PLAT_EARLY_BOOT_ARG_CAPTURE 1

#ifdef X20_EARLY_MARKERS
/*
 * Bounded, pre-C-runtime marker so a failed bring-up can distinguish an
 * image-load/entry failure from an early architectural fault.  The UART is
 * already configured by the preloader at this point.  Bring-up only; off
 * by default (X20_EARLY_MARKERS build flag).
 */
.macro mt6797_early_marker value
	mov	w8, #\value
	ldr	x9, =UART0_BASE
	mov_imm	x10, 100000
1:
	ldr	w11, [x9, #0x14]
	tbz	w11, #5, 2f
	str	w8, [x9]
	dsb	sy
	b	3f
2:
	subs	x10, x10, #1
	b.ne	1b
3:
.endm
#else
.macro mt6797_early_marker value
.endm
#endif

/*
 * Generic hook name consumed by bl31/aarch64/bl31_entrypoint.S.  Defined
 * here so the MT6797 capture lives entirely inside plat/mediatek/mt6797.
 */
.macro plat_early_boot_arg_capture
	mt6797_early_marker 0x41	/* A: BL31 entry reached */
	uxtw	x24, w4
	uxtw	x25, w5
	uxtw	x26, w6
	uxtw	x27, w7
	ldr	x8, =mt6797_boot_argument_location
	str	x24, [x8]
	ldr	x8, =mt6797_boot_argument_size
	str	x25, [x8]
	ldr	x8, =mt6797_boot_bl33_entrypoint
	str	x26, [x8]
	ldr	x8, =mt6797_boot_tee_argument
	str	x27, [x8]
.endm

#endif /* __ASSEMBLER__ */

#endif /* PLAT_EARLY_ENTRY_MACROS_H */
