/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch.h>
#include <lib/psci/psci.h>
#include <platform_def.h>

static const unsigned char mt6797_power_domain_tree_desc[] = {
	PLATFORM_SYSTEM_COUNT,
	PLATFORM_CLUSTER_COUNT,
	PLATFORM_CLUSTER0_CORE_COUNT,
	PLATFORM_CLUSTER1_CORE_COUNT,
	PLATFORM_CLUSTER2_CORE_COUNT,
};

const unsigned char *plat_get_power_domain_tree_desc(void)
{
	return mt6797_power_domain_tree_desc;
}

int plat_core_pos_by_mpidr(u_register_t mpidr)
{
	unsigned int cluster_id;
	unsigned int cpu_id;

	mpidr &= MPIDR_AFFINITY_MASK;
	if (mpidr & ~(MPIDR_CLUSTER_MASK | MPIDR_CPU_MASK))
		return -1;

	cluster_id = MPIDR_AFFLVL1_VAL(mpidr);
	cpu_id = MPIDR_AFFLVL0_VAL(mpidr);
	if (cluster_id >= PLATFORM_CLUSTER_COUNT)
		return -1;

	if ((cluster_id == 2U && cpu_id >= PLATFORM_CLUSTER2_CORE_COUNT) ||
	    (cluster_id != 2U && cpu_id >= PLATFORM_MAX_CPUS_PER_CLUSTER))
		return -1;

	return (cluster_id * PLATFORM_MAX_CPUS_PER_CLUSTER) + cpu_id;
}
