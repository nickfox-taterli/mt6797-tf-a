/* SPDX-License-Identifier: BSD-3-Clause */
/* Opt-in EL3 watchdog capture. No non-secure memory dereferences or locks
 * belong in the FIQ path. The independent TOPRGU final stage performs reset.
 */
#include <arch_helpers.h>
#include <bl31/interrupt_mgmt.h>
#include <common/debug.h>
#include <common/runtime_svc.h>
#include <lib/el3_runtime/context_mgmt.h>
#include <lib/utils.h>
#include <lib/mmio.h>
#include <plat/common/platform.h>
#include <plat_private.h>
#include <platform_def.h>

#define CAP_BASE (RAM_CONSOLE_BASE + UL(0x140))
#define CAP_BYTES UL(0x2c0)
#define CAP_MAGIC ULL(0x3150414354445758) /* XWDTCAP1 */
#define CPU_MAGIC ULL(0x3155504354445758) /* XWD TCPU1 */
#define WDT_INT U(169) /* BSP SPI137 */
#define CAP_SGI U(15)
#define CAP_ABI U(1)
#define CAP_QUERY U(0x57444331)

static unsigned int online_mask;

static volatile uint64_t *cap_words(void)
{
	return (volatile uint64_t *)CAP_BASE;
}

static void capture_cpu(void *handle)
{
	cpu_context_t *ctx = handle;
	unsigned int cpu = plat_my_core_pos();
	volatile uint64_t *record = cap_words() + 8U + 8U * cpu;
	uint64_t spsr = read_ctx_reg(get_el3state_ctx(ctx), CTX_SPSR_EL3);

	record[0] = 0U;
	record[1] = read_mpidr_el1();
	record[2] = read_cntpct_el0();
	record[3] = read_ctx_reg(get_el3state_ctx(ctx), CTX_ELR_EL3);
	record[4] = spsr;
	record[5] = read_ctx_reg(get_gpregs_ctx(ctx), CTX_GPREG_LR);
	record[6] = (spsr & U(1)) != 0U ? read_sp_el1() :
		read_ctx_reg(get_gpregs_ctx(ctx), CTX_GPREG_SP_EL0);
	record[7] = read_ctx_reg(get_gpregs_ctx(ctx), CTX_GPREG_X29);
	dsbsy();
	record[0] = CPU_MAGIC;
	flush_dcache_range((uintptr_t)record, U(64));
	dsbsy();
}

static void print_capture(const char *label)
{
	volatile uint64_t *header = cap_words();
	unsigned int cpu;

	NOTICE("X20 WDT FIQ %s: cnt=%lu mpidr=%lx status=%lx mode=%lx length=%lx mask=%lx\n",
	       label, header[1], header[2], header[3], header[4], header[5], header[6]);
	for (cpu = 0U; cpu < PLATFORM_CORE_COUNT; cpu++) {
		volatile uint64_t *r = header + 8U + 8U * cpu;
		if (r[0] != CPU_MAGIC) {
			NOTICE("X20 WDT FIQ cpu%u absent\n", cpu);
			continue;
		}
		NOTICE("X20 WDT FIQ cpu%u pc=%016lx lr=%016lx sp=%016lx fp=%016lx pstate=%lx cnt=%lu\n",
		       cpu, r[3], r[5], r[6], r[7], r[4], r[2]);
	}
}

static uint64_t capture_fiq(uint32_t id, uint32_t flags, void *handle,
			    void *cookie)
{
	unsigned int irq = plat_ic_acknowledge_interrupt();
	volatile uint64_t *header = cap_words();
	unsigned int cpu;
	uint64_t deadline;

	(void)id;
	(void)cookie;
	if ((irq != WDT_INT && irq != CAP_SGI) ||
	    !is_caller_non_secure(flags)) {
		if (irq < U(1020))
			plat_ic_end_of_interrupt(irq);
		return 0U;
	}
	if (irq == WDT_INT) {
		/* Each CPU owns a separate cache line after this initialization. */
		for (cpu = 0U; cpu < CAP_BYTES / sizeof(uint64_t); cpu++)
			header[cpu] = 0U;
		header[1] = read_cntpct_el0();
		header[2] = read_mpidr_el1();
		header[3] = mmio_read_32(RGU_BASE + U(0x0c));
		header[4] = mmio_read_32(RGU_BASE);
		header[5] = mmio_read_32(RGU_BASE + U(4));
		header[6] = online_mask;
		header[7] = CAP_ABI;
		header[0] = CAP_MAGIC;
		flush_dcache_range(CAP_BASE, CAP_BYTES);
		dsbsy();
	}
	capture_cpu(handle);
	plat_ic_end_of_interrupt(irq);
	if (irq == WDT_INT) {
		for (cpu = 0U; cpu < PLATFORM_CORE_COUNT; cpu++) {
			u_register_t mpidr = ((cpu / 4U) << 8) | (cpu % 4U);
			if (cpu != plat_my_core_pos() && (online_mask & (U(1) << cpu)) != 0U)
				plat_ic_raise_el3_sgi(CAP_SGI, mpidr);
		}
		/* Bound collection to 100 ms. An unresponsive CPU stays absent. */
		deadline = read_cntpct_el0() + SYS_COUNTER_FREQ_IN_TICKS / U(10);
		while (read_cntpct_el0() < deadline)
			isb();
		inv_dcache_range(CAP_BASE, CAP_BYTES);
		print_capture("bark");
	}
	/* Do not resume Linux or allow another owner to reload the final stage. */
	for (;;)
		wfi();
}

void mt6797_wdt_capture_init(void)
{
	uint32_t flags = 0U;

	inv_dcache_range(CAP_BASE, CAP_BYTES);
	if (cap_words()[0] == CAP_MAGIC && cap_words()[7] == CAP_ABI)
		print_capture("previous");
	set_interrupt_rm_flag(flags, NON_SECURE);
	if (register_interrupt_type_handler(INTR_TYPE_EL3, capture_fiq, flags) != 0)
		panic();
	NOTICE("X20 WDT FIQ capture available; SPI169 remains non-secure until armed\n");
}

int mt6797_wdt_capture_control(u_register_t command, u_register_t mask)
{
	unsigned int i;
	volatile uint64_t *header = cap_words();

	if (command == 0U)
		return CAP_QUERY;
	if (command != 1U || mask == 0U || (mask & ~U(0x3ff)) != 0U)
		return -1;
	inv_dcache_range(CAP_BASE, CAP_BYTES);
	if (header[0] != CAP_MAGIC) {
		for (i = 0U; i < CAP_BYTES / sizeof(uint64_t); i++)
			if (header[i] != 0U)
				return -2; /* Never overwrite an unknown SRAM layout. */
	}
	online_mask = (unsigned int)mask;
	plat_ic_disable_interrupt(WDT_INT);
	plat_ic_clear_interrupt_pending(WDT_INT);
	plat_ic_set_interrupt_type(WDT_INT, INTR_TYPE_EL3);
	plat_ic_set_interrupt_priority(WDT_INT, U(0));
	plat_ic_set_spi_routing(WDT_INT, INTR_ROUTING_MODE_PE, U(0));
	/* SGI-only boot properties do not enable distributor Group0. */
	mmio_setbits_32(MT_GIC_BASE, U(1));
	plat_ic_enable_interrupt(WDT_INT);
	dsbsy();
	return 0;
}
