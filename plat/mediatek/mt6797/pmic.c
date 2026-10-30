/*
 * Copyright (c) 2026, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <stdint.h>

#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <lib/mmio.h>
#include <lib/utils_def.h>

#include <plat_private.h>
#include <platform_def.h>

#define PWRAP_BASE		(IO_PHYS + 0x0000d000)
#define WACS2_CMD		(PWRAP_BASE + 0xa0)
#define WACS2_RDATA		(PWRAP_BASE + 0xa4)
#define WACS2_VLDCLR		(PWRAP_BASE + 0xa8)
#define WACS_INIT_DONE		BIT_32(21)
#define WACS_FSM(v)		(((v) >> 16) & 7U)
#define WACS_IDLE		0U
#define WACS_VALID		6U
#define PWRAP_TIMEOUT_US		10000U

#define PMIC_STRUP_CON1		0x0002U
#define PMIC_USBDL		BIT_32(0)
#define PMIC_CHR_CON16		0x0f98U
#define PMIC_USBDL_SET		BIT_32(3)
#define PMIC_USBDL_RST		BIT_32(2)

#define RTC_BBPU			0x4000U
#define RTC_BBPU_KEY		0x4300U
#define RTC_BBPU_AUTO		BIT_32(3)
#define RTC_BBPU_PWREN		BIT_32(0)
#define RTC_BBPU_CBUSY		BIT_32(6)
#define RTC_AL_SEC		0x4018U
#define RTC_2SEC_EN		BIT_32(8)
#define RTC_AUTO_PDN_SEL		BIT_32(6)
#define RTC_PDN1			0x402cU
#define RTC_GPIO_USERS		0x1f00U
#define RTC_SPAR0		0x4030U
#define RTC_PROT			0x4036U
#define RTC_UNLOCK1		0x586aU
#define RTC_UNLOCK2		0x9136U
#define RTC_WRTGR		0x403cU
#define RTC_CON			0x403eU
#define RTC_F32KOB		BIT_32(5)

/*
 * SYSTEM_OFF has stopped the non-secure CPUs. Reuse the initialized WACS2
 * channel; never reinitialize PWRAP or change its clocks/arbitration here.
 * The command layout and FSM handshake match the MT6797 BSP pwrap HAL.
 */
static int pwrap_wait(unsigned int state)
{
	for (unsigned int i = 0; i < PWRAP_TIMEOUT_US; i++) {
		uint32_t status = mmio_read_32(WACS2_RDATA);

		if ((status & WACS_INIT_DONE) == 0U)
			return -ENODEV;
		if (WACS_FSM(status) == state)
			return 0;
		/* Drain a read interrupted when Linux entered SYSTEM_OFF. */
		if (state == WACS_IDLE && WACS_FSM(status) == WACS_VALID)
			mmio_write_32(WACS2_VLDCLR, 1U);
		udelay(1);
	}
	ERROR("PWRAP power-off timeout: state=%u status=0x%08x\n",
	      state, mmio_read_32(WACS2_RDATA));
	return -ETIMEDOUT;
}

static int pmic_write(unsigned int reg, unsigned int value)
{
	int ret = pwrap_wait(WACS_IDLE);

	if (ret != 0)
		return ret;
	mmio_write_32(WACS2_CMD, BIT_32(31) | ((reg >> 1) << 16) | value);
	return 0;
}

static int pmic_read(unsigned int reg, unsigned int *value)
{
	int ret = pwrap_wait(WACS_IDLE);

	if (ret != 0)
		return ret;
	mmio_write_32(WACS2_CMD, (reg >> 1) << 16);
	ret = pwrap_wait(WACS_VALID);
	if (ret != 0)
		return ret;
	*value = mmio_read_32(WACS2_RDATA) & 0xffffU;
	mmio_write_32(WACS2_VLDCLR, 1U);
	return 0;
}

static int rtc_write(unsigned int reg, unsigned int value)
{
	unsigned int bbpu;
	int ret = pmic_write(reg, value);

	if (ret != 0)
		return ret;
	ret = pmic_write(RTC_WRTGR, 1U);
	if (ret != 0)
		return ret;
	for (unsigned int i = 0; i < 100U; i++) {
		ret = pmic_read(RTC_BBPU, &bbpu);
		if (ret != 0)
			return ret;
		if ((bbpu & RTC_BBPU_CBUSY) == 0U)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

int mt6797_pmic_power_off(void)
{
	unsigned int al_sec, pdn1, spar0, con, check;
	int ret;

	ret = pmic_read(RTC_AL_SEC, &al_sec);
	if (ret != 0)
		return ret;
	ret = pmic_read(RTC_PDN1, &pdn1);
	if (ret != 0)
		return ret;
	ret = pmic_read(RTC_SPAR0, &spar0);
	if (ret != 0)
		return ret;
	ret = pmic_read(RTC_CON, &con);
	if (ret != 0)
		return ret;
	NOTICE("MT6351 power-off: AL_SEC=%04x PDN1=%04x SPAR0=%04x CON=%04x\n",
	       al_sec, pdn1, spar0, con);

	ret = rtc_write(RTC_PROT, RTC_UNLOCK1);
	if (ret != 0)
		return ret;
	ret = rtc_write(RTC_PROT, RTC_UNLOCK2);
	if (ret != 0)
		return ret;

	/* BSP rtc_disable_2sec_reboot(), preserving the alarm seconds. */
	al_sec &= ~(RTC_2SEC_EN | RTC_AUTO_PDN_SEL);
	ret = rtc_write(RTC_AL_SEC, al_sec);
	if (ret != 0)
		return ret;
	ret = pmic_read(RTC_AL_SEC, &check);
	if (ret != 0)
		return ret;
	if (check != al_sec)
		return -EIO;

	/* BSP hal_rtc_bbpu_pwdn(): retain 32 kHz for registered RTC users. */
	if ((pdn1 & RTC_GPIO_USERS) == 0U) {
		con |= RTC_F32KOB;
		ret = rtc_write(RTC_CON, con);
		if (ret != 0)
			return ret;
		ret = pmic_read(RTC_CON, &check);
		if (ret != 0)
			return ret;
		if (check != con)
			return -EIO;
	}

	/*
	 * The BSP charging initialization leaves USB-download power mode with
	 * USBDL_SET=0 followed by USBDL_RST=1. Our batteryless Linux does not
	 * run that initialization. Do it only at SYSTEM_OFF, with BBPU still
	 * high, so the download latch cannot keep the PMIC powered afterward.
	 * Preserve all other charger fields; never enable battery charging.
	 */
	ret = pmic_read(PMIC_STRUP_CON1, &check);
	if (ret != 0)
		return ret;
	NOTICE("MT6351 power-off: USBDL=%u\n", check & PMIC_USBDL);
	if ((check & PMIC_USBDL) != 0U) {
		ret = pmic_read(PMIC_CHR_CON16, &check);
		if (ret != 0)
			return ret;
		ret = pmic_write(PMIC_CHR_CON16, check & ~PMIC_USBDL_SET);
		if (ret != 0)
			return ret;
		ret = pmic_read(PMIC_CHR_CON16, &check);
		if (ret != 0)
			return ret;
		ret = pmic_write(PMIC_CHR_CON16, check | PMIC_USBDL_RST);
		if (ret != 0)
			return ret;
		ret = pmic_read(PMIC_STRUP_CON1, &check);
		if (ret != 0)
			return ret;
		NOTICE("MT6351 power-off: USBDL status after request=%u\n", check & PMIC_USBDL);
		/* The BSP still requests BBPU power-down if this status stays high. */
	}

	/*
	 * Do not copy the BSP's GPIO148 SRCLKENA workaround: on MT6797 that
	 * pad is AUD_DAT, not SRCLKENA. No SoC pin mux is changed here.
	 * rtc_bbpu_pwrdown(true) clears BBPU while retaining alarm wakeup.
	 */
	NOTICE("MT6351 power-off: BBPU=4309 (2sec disabled, RTC users=%04x)\n",
	       pdn1 & RTC_GPIO_USERS);
	ret = rtc_write(RTC_BBPU, RTC_BBPU_KEY | RTC_BBPU_AUTO | RTC_BBPU_PWREN);
	if (ret != 0)
		return ret;
	mdelay(1000);
	ret = pmic_read(RTC_BBPU, &check);
	if (ret != 0)
		return ret;
	ERROR("MT6351 still running: BBPU=%04x (requested 4309)\n", check);
	return -ETIMEDOUT;
}
