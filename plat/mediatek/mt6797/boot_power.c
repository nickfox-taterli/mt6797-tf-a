/* SPDX-License-Identifier: BSD-3-Clause */
/* Fixed X20 DA9214 bootstrap. Called before Linux owns I2C6/regulators. */
#include <stdbool.h>
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

/* Controller setup for the high-speed push-pull FIFO transaction that the
 * proven kernel path also uses. */
#define I2C_CTRL_HS_PUSH_PULL U(0x2a)
#define I2C_CTRL_READ BIT_32(4)
#define I2C_TIMING_HS U(0x10)
#define I2C_HS_DIVIDER U(0x0201)
#define DA9214_ADDR_BYTE (U(0x68) << 1)
#define I2C_FIFO_COUNT GENMASK_32(7, 4)
/* One completion poll per microsecond; ~10 ms bounds a stuck transaction. */
#define BUCK_COMPLETE_POLLS 10000U

/* Program one I2C6 FIFO transaction to the DA9214 and start it.  For a read
 * the single-byte read length carries the register address from DATA_PORT;
 * for a write both the register and the value go through DATA_PORT. */
static void buck_prepare_fifo(uint8_t reg, const uint8_t *value, bool read)
{
	mmio_write_16(I2C6 + CONTROL,
		      I2C_CTRL_HS_PUSH_PULL | (read ? I2C_CTRL_READ : 0U));
	mmio_write_16(I2C6 + TIMING, I2C_TIMING_HS);
	mmio_write_16(I2C6 + HS, I2C_HS_DIVIDER);
	mmio_write_16(I2C6 + IO_CONFIG, 0U);
	mmio_write_16(I2C6 + SLAVE_ADDR, DA9214_ADDR_BYTE);
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
}

/* Poll the transfer to completion.  Returns true when the transaction
 * finished successfully, with the read byte stored in *value for a read.
 * ACK error, timeout, or a read completion with an empty RX FIFO all return
 * false and leave the transaction status in *status for the failure log. */
static bool buck_wait_complete(uint8_t *value, bool read, uint16_t *status)
{
	unsigned int i;

	for (i = 0U; i < BUCK_COMPLETE_POLLS; i++) {
		*status = mmio_read_16(I2C6 + INTR_STAT);
		if ((*status & ACK_ERROR) != 0U)
			return false;
		if ((*status & COMPLETE) != 0U) {
			if (read) {
				if (((mmio_read_16(I2C6 + FIFO_STAT) &
				      I2C_FIFO_COUNT) >> 4) < 1U)
					return false;
				*value = (uint8_t)mmio_read_16(I2C6 + DATA_PORT);
			}
			return true;
		}
		udelay(1);
	}
	return false;
}

/* Failure cleanup: reset the controller and clear the pending events so the
 * next transaction starts from a known state. */
static void buck_reset_fifo(void)
{
	mmio_write_16(I2C6 + SOFTRESET, 1U);
	mmio_write_16(I2C6 + INTR_STAT, ACK_ERROR | COMPLETE);
	mmio_write_16(I2C6 + FIFO_ADDR_CLR, 1U);
}

static int buck_transfer(uint8_t reg, uint8_t *value, bool read)
{
	uint16_t status = 0U;

	buck_prepare_fifo(reg, value, read);
	if (!buck_wait_complete(value, read, &status)) {
		buck_reset_fifo();
		ERROR("X20 boot buck: I2C6 reg=%x read=%u status=%x\n",
		      reg, read, status);
		return PSCI_E_INTERN_FAIL;
	}
	return PSCI_E_SUCCESS;
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
