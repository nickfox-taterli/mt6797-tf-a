/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch.h>
#include <asm_macros.S>
#include <platform_def.h>

	.globl plat_is_my_cpu_primary
	.globl plat_my_core_pos
	.globl platform_mem_init
	.globl plat_reset_handler
	.globl plat_set_my_stack
	.globl mt6797_early_uart_putc
	.globl mt6797_zeromem

/* UART output usable before the C console and MMU are initialized.
 * Single-point gate: every marker call site stays, the function itself
 * collapses to a bare return unless X20_EARLY_MARKERS requested the
 * bring-up markers. */
func mt6797_early_uart_putc
#ifndef X20_EARLY_MARKERS
	ret
#else
	/* x0 contains the byte to emit; x12-x15 are scratch. */
	mov	w12, w0
	ldr	x13, =UART0_BASE
	mov_imm	x14, 100000
1:
	ldr	w15, [x13, #0x14]
	/* Wait until the previous byte has left the transmitter. */
	tbz	w15, #6, 2f
	str	w12, [x13]
	dsb	sy
	mov_imm	x14, 100000
3:
	ldr	w15, [x13, #0x14]
	tbz	w15, #6, 4f
	ret
4:
	subs	x14, x14, #1
	b.ne	3b
5:
	ret
2:
	subs	x14, x14, #1
	b.ne	1b
	ret
#endif /* X20_EARLY_MARKERS */
endfunc mt6797_early_uart_putc

/*
 * The Vendor MT6797 entrypoint zeros trusted SRAM with ordinary stores while
 * the MMU is off.  Keep that property for the reset-to-BL31 path instead of
 * relying on the generic cache-zeroing helper.
 */
func mt6797_zeromem
	mov	x9, x30
	mov	x4, x0
	mov	x5, x1
	mov	w0, #0x49 /* I: legacy zeroing entered */
	bl	mt6797_early_uart_putc
	mov	x0, x4
	mov	x1, x5
	add	x2, x0, x1
1:
	sub	x3, x2, x0
	cmp	x3, #8
	b.lt	2f
	str	xzr, [x0], #8
	b	1b
2:
	cmp	x0, x2
	b.eq	3f
	strb	 wzr, [x0], #1
	b	2b
3:
	mov	x4, x0
	mov	x5, x1
	mov	w0, #0x4a /* J: legacy zeroing returned */
	bl	mt6797_early_uart_putc
	mov	x0, x4
	mov	x1, x5
	ret	x9
endfunc mt6797_zeromem

func plat_reset_handler
	mov	w0, #0x43 /* C: reset handler reached */
	b	mt6797_early_uart_putc
endfunc plat_reset_handler

/* Keep the common MP-stack calculation, but mark its successful return. */
func plat_set_my_stack
	mov	x9, x30
	mov	w0, #0x4c /* L: platform stack setup entered */
	bl	mt6797_early_uart_putc
	bl	plat_get_my_stack
	mov	x10, x0
	mov	w0, #0x4f /* O: platform stack address returned */
	bl	mt6797_early_uart_putc
	mov	sp, x10
	mov	w0, #0x4d /* M: platform stack pointer installed */
	bl	mt6797_early_uart_putc
	ret	x9
endfunc plat_set_my_stack

func plat_is_my_cpu_primary
	mrs	x0, mpidr_el1
	and	x0, x0, #(MPIDR_CLUSTER_MASK | MPIDR_CPU_MASK)
	cmp	x0, #PLAT_PRIMARY_CPU
	cset	x0, eq
	ret
endfunc plat_is_my_cpu_primary

/* Return linear position: cluster 0 = 0..3, cluster 1 = 4..7,
 * cluster 2 = 8..9. */
func plat_my_core_pos
	mrs	x0, mpidr_el1
	and	x1, x0, #MPIDR_CPU_MASK
	ubfx	x0, x0, #MPIDR_AFF1_SHIFT, #8
	add	x0, x1, x0, lsl #2
	ret
endfunc plat_my_core_pos

/* RESET_TO_BL31 calls this hook before the generic C runtime is initialized.
 * There is no secondary holding pen on the programmable-reset path yet. */
func platform_mem_init
	mov	x9, x30
	mov	w0, #0x44 /* D: platform memory init reached */
	bl	mt6797_early_uart_putc
	ret	x9
endfunc platform_mem_init
