/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch.h>
#include <assert.h>
#include <stdint.h>

#include <drivers/arm/cci400.h>
#include <lib/mmio.h>

#define CCI400_MAX_CLUSTERS	3U

static uintptr_t cci_base_addr;
static unsigned int cci_cluster_to_iface[CCI400_MAX_CLUSTERS];

void mcsi_a_init(uintptr_t base, unsigned int cluster0_iface,
		 unsigned int cluster1_iface, unsigned int cluster2_iface)
{
	assert(base != 0U);
	assert(cluster0_iface >= 3U && cluster0_iface <= 5U);
	assert(cluster1_iface >= 3U && cluster1_iface <= 5U);
	assert(cluster2_iface >= 3U && cluster2_iface <= 5U);
	assert(cluster0_iface != cluster1_iface);
	assert(cluster0_iface != cluster2_iface);
	assert(cluster1_iface != cluster2_iface);

	cci_base_addr = base;
	cci_cluster_to_iface[0] = CCI400_SLAVE_IFACE_OFFSET(cluster0_iface);
	cci_cluster_to_iface[1] = CCI400_SLAVE_IFACE_OFFSET(cluster1_iface);
	cci_cluster_to_iface[2] = CCI400_SLAVE_IFACE_OFFSET(cluster2_iface);
}

static uintptr_t get_slave_iface_base(uintptr_t mpidr)
{
	unsigned int cluster_id =
		(mpidr >> MPIDR_AFF1_SHIFT) & MPIDR_AFFLVL_MASK;

	assert(cluster_id < CCI400_MAX_CLUSTERS);
	assert(cci_cluster_to_iface[cluster_id] != 0U);

	return cci_base_addr + cci_cluster_to_iface[cluster_id];
}

void mcsi_a_enable_cluster_dcm(uintptr_t base)
{
	assert(base != 0U);
	mmio_write_32(base, mmio_read_32(base) | 0xffff0000U);
}

void cci_enable_cluster_coherency(uintptr_t mpidr)
{
	uintptr_t iface_base;

	assert(cci_base_addr != 0U);
	iface_base = get_slave_iface_base(mpidr);
	mmio_write_32(iface_base + CCI400_SNOOP_CTRL_REG,
		      CCI400_DVM_EN_BIT | CCI400_SNOOP_EN_BIT);

	while ((mmio_read_32(cci_base_addr + CCI400_STATUS_REG) &
		CCI400_CHANGE_PENDING_BIT) != 0U)
		;
}

void cci_disable_cluster_coherency(uintptr_t mpidr)
{
	uintptr_t iface_base;

	assert(cci_base_addr != 0U);
	iface_base = get_slave_iface_base(mpidr);
	mmio_write_32(iface_base + CCI400_SNOOP_CTRL_REG,
		      ~(CCI400_DVM_EN_BIT | CCI400_SNOOP_EN_BIT));

	while ((mmio_read_32(cci_base_addr + CCI400_STATUS_REG) &
		CCI400_CHANGE_PENDING_BIT) != 0U)
		;
}
