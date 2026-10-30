/* SPDX-License-Identifier: BSD-3-Clause */
/* Fixed X20 DA9214 bootstrap. Called before Linux owns I2C6/regulators. */
#include <stdint.h>
#include <arch_helpers.h>
#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <lib/utils_def.h>
#include <plat_private.h>

#define I2C6 UL(0x1100e000)
#define DATA_PORT 0x000U
#define SLAVE_ADDR 0x004U
#define INTR_MASK 0x008U
#define INTR_STAT 0x00cU
#define CONTROL 0x010U
#define TRANSFER_LEN 0x014U
#define TRANSAC_LEN 0x018U
#define TIMING 0x020U
#define START 0x024U
#define FIFO_STAT 0x030U
#define FIFO_ADDR_CLR 0x038U
#define IO_CONFIG 0x040U
#define HS 0x048U
#define SOFTRESET 0x050U
#define COMPLETE BIT_32(0)
#define ACK_ERROR (BIT_32(1) | BIT_32(2))

static int buck_transfer(uint8_t reg, uint8_t *value, bool read)
{
	uint16_t status = 0U;

	/* Same high-speed push-pull FIFO transaction as the proven kernel path. */
	mmio_write_16(I2C6 + CONTROL, U(0x2a) | (read ? BIT_32(4) : 0U));
	mmio_write_16(I2C6 + TIMING, U(0x10));
	mmio_write_16(I2C6 + HS, U(0x0201));
	mmio_write_16(I2C6 + IO_CONFIG, 0U);
	mmio_write_16(I2C6 + SLAVE_ADDR, U(0x68) << 1);
	mmio_write_16(I2C6 + INTR_MASK,
		      mmio_read_16(I2C6 + INTR_MASK) & ~(ACK_ERROR | COMPLETE));
	mmio_write_16(I2C6 + INTR_STAT, ACK_ERROR | COMPLETE);
	mmio_write_16(I2C6 + FIFO_ADDR_CLR, 1U);
	mmio_write_16(I2C6 + TRANSFER_LEN, read ? U(0x101) : 2U);
	mmio_write_16(I2C6 + TRANSAC_LEN, read ? 2U : 1U);
	mmio_write_16(I2C6 + DATA_PORT, reg);
	if (!read)
		mmio_write_16(I2C6 + DATA_PORT, *value);
	dsbsy();
	mmio_write_16(I2C6 + START, 1U);
	for (unsigned int i = 0U; i < 10000U; i++) {
		status = mmio_read_16(I2C6 + INTR_STAT);
		if ((status & ACK_ERROR) != 0U)
			break;
		if ((status & COMPLETE) != 0U) {
			if (read) {
				if (((mmio_read_16(I2C6 + FIFO_STAT) >> 4) & U(0xf)) < 1U)
					break;
				*value = (uint8_t)mmio_read_16(I2C6 + DATA_PORT);
			}
			return PSCI_E_SUCCESS;
		}
		udelay(1);
	}
	mmio_write_16(I2C6 + SOFTRESET, 1U);
	mmio_write_16(I2C6 + INTR_STAT, ACK_ERROR | COMPLETE);
	mmio_write_16(I2C6 + FIFO_ADDR_CLR, 1U);
	ERROR("X20 boot buck: I2C6 reg=%x read=%u status=%x\n", reg, read, status);
	return PSCI_E_INTERN_FAIL;
}

int mt6797_boot_buck_enable(void)
{
	uint8_t page, buck;
	int rc = buck_transfer(0U, &page, true);

	if (rc != PSCI_E_SUCCESS)
		return rc;
	page &= ~U(0xf);
	rc = buck_transfer(0U, &page, false);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	rc = buck_transfer(U(0x5e), &buck, true);
	if (rc != PSCI_E_SUCCESS)
		return rc;
	buck |= BIT_32(0);
	return buck_transfer(U(0x5e), &buck, false);
}
