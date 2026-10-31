#!/usr/bin/env python3
"""Compile the real X20 DA9214 bootstrap and replay its I2C6 transactions.

plat/mediatek/mt6797/boot_power.c is compiled unmodified; only the MMIO and
delay primitives are replaced by a logged I2C6 register model, so the tested
transaction order is the shipped one, not a re-implementation.

Covers: the exact register write order and payload of a read and a write
transaction, the four-transaction buck enable sequence with derived values,
completion delay via a countdown model, ACK-error and timeout cleanup with
the reset register sequence, and a read completion with an empty RX FIFO.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UNIT = ROOT / "plat/mediatek/mt6797/boot_power.c"

SHIM_FILES = {
    "arch_helpers.h": """static void dsbsy(void) { extern void record_dsbsy(void); record_dsbsy(); }
__attribute__((unused)) static void dsbsy_ref_(void) { dsbsy(); }
""",
    "common/debug.h": """#include <stdio.h>
#include <string.h>
extern char error_log[512];
#define ERROR(...) do { char *p = error_log + strlen(error_log); \\
    snprintf(p, sizeof(error_log) - strlen(error_log), __VA_ARGS__); } while (0)
""",
    "drivers/delay_timer.h": """extern void udelay(unsigned int us);
""",
    "lib/mmio.h": """#include <stdint.h>
uint16_t mmio_read_16(uintptr_t addr);
void mmio_write_16(uintptr_t addr, uint16_t value);
""",
    "lib/psci/psci.h": """#define PSCI_E_SUCCESS 0
#define PSCI_E_INTERN_FAIL -6
""",
    "lib/utils_def.h": """#define U(x) x##U
#define UL(x) x##UL
#define BIT_32(x) (1U << (x))
#define GENMASK_32(h, l) (((~0U) - (1U << (l)) + 1) & (~0U >> (31 - (h))))
""",
    "plat_private.h": """#include <stdint.h>
int mt6797_boot_buck_enable(void);
""",
}

STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define I2C6 0x1100e000u
#define DATA_PORT 0x000u
#define SLAVE_ADDR 0x004u
#define INTR_MASK 0x008u
#define INTR_STAT 0x00cu
#define CONTROL 0x010u
#define TRANSFER_LEN 0x014u
#define TRANSAC_LEN 0x018u
#define TIMING 0x020u
#define START 0x024u
#define FIFO_STAT 0x030u
#define FIFO_ADDR_CLR 0x038u
#define IO_CONFIG 0x040u
#define HS 0x048u
#define SOFTRESET 0x050u

char error_log[512];

/* ---- operation log -------------------------------------------------- */
enum op_kind { OP_READ, OP_WRITE };
struct op { enum op_kind kind; unsigned int addr; unsigned int value; };
static struct op ops[16384];
static unsigned int op_count;
static unsigned int dsbsy_count, udelay_count;

static void log_op(enum op_kind kind, unsigned int addr, unsigned int value)
{
    assert(op_count < sizeof(ops) / sizeof(ops[0]));
    ops[op_count++] = (struct op){ kind, addr, value };
}

/* ---- fault injection / model ---------------------------------------- */
enum complete_mode { COMPLETE_OK, COMPLETE_ACK_ERR, COMPLETE_NEVER };
static enum complete_mode mode = COMPLETE_OK;
static unsigned int complete_after_polls = 3u;
static bool empty_fifo_on_complete;
static uint16_t intr_mask_shadow = 0xffffu;
static uint16_t read_data_port_value = 0x5u;    /* page register */
static uint16_t buck_read_value = 0x40u;        /* buck register 0x5e */
static uint8_t last_reg;                        /* last DATA_PORT write */
static unsigned int polls_done, starts_seen;

uint16_t mmio_read_16(uintptr_t addr)
{
    log_op(OP_READ, (unsigned int)addr, 0);
    switch (addr - I2C6) {
    case INTR_MASK:
        return intr_mask_shadow;
    case INTR_STAT: {
        if (starts_seen == 0u)
            return 0;                       /* idle before any start */
        polls_done++;
        if (polls_done >= complete_after_polls) {
            if (mode == COMPLETE_ACK_ERR)
                return (1u << 1) | (1u << 2);
            if (mode == COMPLETE_OK)
                return 1u;                  /* COMPLETE */
        }
        return 0;
    }
    case FIFO_STAT:
        /* count in bits [7:4]; zero means the RX FIFO is empty */
        return empty_fifo_on_complete ? 0u : (1u << 4);
    case DATA_PORT:
        return last_reg == 0x5eu ? buck_read_value : read_data_port_value;
    default:
        return 0;
    }
}

void mmio_write_16(uintptr_t addr, uint16_t value)
{
    log_op(OP_WRITE, (unsigned int)addr, value);
    switch (addr - I2C6) {
    case INTR_MASK:
        intr_mask_shadow = value;
        return;
    case DATA_PORT:
        last_reg = (uint8_t)value;
        return;
    case START:
        if (value == 1u) {
            starts_seen++;
            polls_done = 0;
        }
        return;
    default:
        return;
    }
}

void record_dsbsy(void);
void record_dsbsy(void) { dsbsy_count++; }

int mt6797_boot_buck_enable(void);
void udelay(unsigned int us) { (void)us; udelay_count++; }

"""

MAIN = r"""
static void expect_prepare_read(uint8_t reg, unsigned int o)
{
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + CONTROL &&
           ops[o].value == (0x2au | (1u << 4)));
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + TIMING &&
           ops[o].value == 0x10u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + HS &&
           ops[o].value == 0x0201u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + IO_CONFIG &&
           ops[o].value == 0u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + SLAVE_ADDR &&
           ops[o].value == (0x68u << 1));
    o++;
    assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_MASK);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + INTR_MASK &&
           ops[o].value == (0xffffu & ~((1u << 1) | (1u << 2) | 1u)));
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + INTR_STAT &&
           ops[o].value == ((1u << 1) | (1u << 2) | 1u));
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + FIFO_ADDR_CLR &&
           ops[o].value == 1u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + TRANSFER_LEN &&
           ops[o].value == 0x101u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + TRANSAC_LEN &&
           ops[o].value == 2u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + DATA_PORT &&
           ops[o].value == reg);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + START &&
           ops[o].value == 1u);
}

static void expect_prepare_write(uint8_t reg, uint8_t value, unsigned int o)
{
    /* Same setup as a read but with the write CONTROL bit, single-transaction
     * lengths, and the value byte appended after the register byte. */
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + CONTROL &&
           ops[o].value == 0x2au);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + TIMING &&
           ops[o].value == 0x10u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + HS &&
           ops[o].value == 0x0201u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + IO_CONFIG &&
           ops[o].value == 0u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + SLAVE_ADDR &&
           ops[o].value == (0x68u << 1));
    o++;
    assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_MASK);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + INTR_MASK &&
           ops[o].value == (0xffffu & ~((1u << 1) | (1u << 2) | 1u)));
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + INTR_STAT &&
           ops[o].value == ((1u << 1) | (1u << 2) | 1u));
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + FIFO_ADDR_CLR &&
           ops[o].value == 1u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + TRANSFER_LEN &&
           ops[o].value == 2u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + TRANSAC_LEN &&
           ops[o].value == 1u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + DATA_PORT &&
           ops[o].value == reg);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + DATA_PORT &&
           ops[o].value == value);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + START &&
           ops[o].value == 1u);
}

static void expect_cleanup(unsigned int o)
{
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + SOFTRESET &&
           ops[o].value == 1u);
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + INTR_STAT &&
           ops[o].value == ((1u << 1) | (1u << 2) | 1u));
    o++;
    assert(ops[o].kind == OP_WRITE && ops[o].addr == I2C6 + FIFO_ADDR_CLR &&
           ops[o].value == 1u);
}

static void reset_model(void)
{
    memset(ops, 0, sizeof(ops));
    op_count = 0; dsbsy_count = 0; udelay_count = 0;
    mode = COMPLETE_OK; complete_after_polls = 3u;
    empty_fifo_on_complete = false;
    intr_mask_shadow = 0xffffu;
    read_data_port_value = 0x5u;
    buck_read_value = 0x40u;
    last_reg = 0u;
    polls_done = 0; starts_seen = 0;
    memset(error_log, 0, sizeof(error_log));
}

int main(void)
{
    /* Boot buck enable: read page, write cleared page, read buck, write
     * enabled buck.  The model reads return 0x05 and 0x40. */
    reset_model();
    read_data_port_value = 0x5u;
    assert(mt6797_boot_buck_enable() == 0);
    assert(starts_seen == 4u && dsbsy_count == 4u);
    /* walk the four prepared transactions and the derived payloads */
    {
        unsigned int o = 0;
        /* transfer 1: read reg 0 */
        expect_prepare_read(0u, o);
        o += 13;
        /* completion polls and read of DATA_PORT follow */
        while (ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT) o++;
        assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + FIFO_STAT);
        o++;
        assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + DATA_PORT);
        o++;
        /* transfer 2: write reg 0 with 0x05 & ~0x0f == 0x00 */
        expect_prepare_write(0u, 0x00u, o);
        o += 14;
        while (ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT) o++;
        /* transfer 3: read reg 0x5e */
        expect_prepare_read(0x5eu, o);
        o += 13;
        while (ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT) o++;
        assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + FIFO_STAT);
        o++;
        assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + DATA_PORT);
        o++;
        /* transfer 4: write reg 0x5e with 0x40 | 1 */
        expect_prepare_write(0x5eu, 0x41u, o);
        o += 14;
        while (o < op_count) {
            assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT);
            o++;
        }
    }

    /* Delayed completion still succeeds after several polls. */
    reset_model();
    complete_after_polls = 500u;
    read_data_port_value = 0x77u;
    assert(mt6797_boot_buck_enable() == 0);
    /* the completing poll itself does not sleep: 499 sleeps per transfer */
    assert(udelay_count == 4u * 499u);

    /* ACK error: one poll, cleanup sequence, intern failure. */
    reset_model();
    mode = COMPLETE_ACK_ERR;
    complete_after_polls = 1u;
    assert(mt6797_boot_buck_enable() == -6);
    assert(starts_seen == 1u);
    {
        unsigned int o = 13;                /* after the first prepare */
        assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT);
        o++;
        expect_cleanup(o);
        assert(strstr(error_log, "reg=0") && strstr(error_log, "status=6"));
    }

    /* Timeout: exactly BUCK_COMPLETE_POLLS polls, cleanup, intern failure. */
    reset_model();
    mode = COMPLETE_NEVER;
    assert(mt6797_boot_buck_enable() == -6);
    assert(starts_seen == 1u);
    assert(udelay_count == 10000u);
    {
        unsigned int o = 13;
        unsigned int stat_reads = 0;
        while (ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT) {
            o++; stat_reads++;
        }
        assert(stat_reads == 10000u);
        expect_cleanup(o);
    }

    /* Read completion with an empty RX FIFO: cleanup, intern failure. */
    reset_model();
    empty_fifo_on_complete = true;
    assert(mt6797_boot_buck_enable() == -6);
    assert(starts_seen == 1u);
    {
        unsigned int o = 13;
        while (ops[o].kind == OP_READ && ops[o].addr == I2C6 + INTR_STAT) o++;
        assert(ops[o].kind == OP_READ && ops[o].addr == I2C6 + FIFO_STAT);
        o++;
        expect_cleanup(o);
        assert(strstr(error_log, "read=1"));
    }

    puts("Real boot_power.c: I2C6 order/payload, delayed completion,"
         " ACK/timeout/empty-FIFO cleanup: PASS");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="mt6797-buck-") as temp:
        folder = Path(temp)
        shim = folder / "shim"
        shim.mkdir()
        for name, text in SHIM_FILES.items():
            path = shim / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(text)
        unit = folder / "boot_power.c"
        unit.write_text(UNIT.read_text())
        harness = folder / "harness.c"
        harness.write_text(
            "#include <stdint.h>\n#include <lib/mmio.h>\n"
            "#include <arch_helpers.h>\n" + STUBS + "\n" + MAIN)
        # boot_power.c is a separate TU; declare what MAIN drives.
        binary = folder / "test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra",
                        "-Wno-unused-parameter", "-Werror",
                        "-I", str(shim), "-I", str(UNIT.parent),
                        str(unit), str(harness), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
