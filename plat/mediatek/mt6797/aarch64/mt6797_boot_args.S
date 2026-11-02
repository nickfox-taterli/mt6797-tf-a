/*
 * Copyright (c) 2026, MediaTek Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Storage for the boot-ABI values captured by plat_early_boot_arg_capture
 * (see include/plat_early_entry_macros.h).  These locations must not be in
 * .data/.bss: el3_entrypoint_common clears those sections before
 * bl31_main() reads the saved boot ABI values.
 */

	.section .rodata.mt6797_boot_args, "a"
	.align 3
	.global mt6797_boot_argument_location
mt6797_boot_argument_location:
	.quad 0
	.global mt6797_boot_argument_size
mt6797_boot_argument_size:
	.quad 0
	.global mt6797_boot_bl33_entrypoint
mt6797_boot_bl33_entrypoint:
	.quad 0
	.global mt6797_boot_tee_argument
mt6797_boot_tee_argument:
	.quad 0
