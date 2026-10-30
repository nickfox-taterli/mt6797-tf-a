/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MT6797_EMI_MPU_H
#define MT6797_EMI_MPU_H

#include <stdint.h>

/*
 * Only the two ConSys shared-memory windows are exposed to normal world.
 * Arbitrary EMI register writes remain intentionally unsupported.
 */
int mt6797_emi_mpu_set_region(uint64_t start, uint64_t end,
				      uint32_t region_permission);
uint32_t mt6797_emi_mpu_read(uint32_t offset);

#endif /* MT6797_EMI_MPU_H */
