/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef CCI400_H
#define CCI400_H

#define CCI400_SLAVE_IFACE3_OFFSET		0x4000U
#define CCI400_SLAVE_IFACE4_OFFSET		0x5000U
#define CCI400_SLAVE_IFACE5_OFFSET		0x6000U
#define CCI400_SLAVE_IFACE_OFFSET(index)	(0x1000U + (0x1000U * (index)))

#define CCI400_STATUS_REG			0x0cU
#define CCI400_SNOOP_CTRL_REG			0x00U
#define CCI400_CHANGE_PENDING_BIT		(1U << 0)
#define CCI400_DVM_EN_BIT			(1U << 1)
#define CCI400_SNOOP_EN_BIT			(1U << 0)

#ifndef __ASSEMBLER__

#include <stdint.h>

void mcsi_a_init(uintptr_t base, unsigned int cluster0_iface,
		 unsigned int cluster1_iface, unsigned int cluster2_iface);
void mcsi_a_enable_cluster_dcm(uintptr_t base);
void cci_enable_cluster_coherency(uintptr_t mpidr);
void cci_disable_cluster_coherency(uintptr_t mpidr);

#endif /* __ASSEMBLER__ */
#endif /* CCI400_H */
