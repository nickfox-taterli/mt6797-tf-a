#!/usr/bin/env python3
"""Compile the real MT6797 SiP handler and exercise its dispatch decisions.

plat/mediatek/mt6797/plat_sip_calls.c is compiled unmodified; only the SMCCC
plumbing and the platform services it calls are replaced by controllable
stubs, so the tested decisions are the shipped ones, not a re-implementation.

Covers: secure-world rejection, AArch32 argument truncation, OCP read/write
range and alignment checks, the MCUSYS write whitelist, kernel-boot argument
capture for both 32/64-bit ABIs, capability queries, the vendor UUID, and
unknown-SMC handling.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UNIT = ROOT / "plat/mediatek/mt6797/plat_sip_calls.c"
PLAT_INCLUDE = ROOT / "plat/mediatek/mt6797/include"
MTK_INCLUDE = ROOT / "plat/mediatek/include"

SHIM_FILES = {
    "arch_helpers.h": """static void dsb(void) { extern void record_dsb(void); record_dsb(); }
""",
    "bl31/bl31.h": """#include <stdint.h>
void bl31_prepare_kernel_entry(uint64_t k32_64);
""",
    "common/debug.h": """#include <stdio.h>
#define ERROR(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
""",
    "common/runtime_svc.h": """#include <stdint.h>
#include "lib/smccc.h"
#ifndef SHIM_RUNTIME_SVC_H
#define SHIM_RUNTIME_SVC_H
#define OEN_SIP_START 6
#define OEN_SIP_END 6
#define SMC_TYPE_FAST 0x40000000
struct rt_svc_desc {
    uintptr_t (*handler)(uint32_t, u_register_t, u_register_t, u_register_t,
                         u_register_t, void *, void *, u_register_t);
};
#define DECLARE_RT_SVC(_name, _s, _e, _t, _init, _handler) \\
    const struct rt_svc_desc rt_svc_desc_##_name = { .handler = (_handler) };
#endif /* SHIM_RUNTIME_SVC_H */
""",
    "lib/mmio.h": """#include <stdint.h>
uint32_t mmio_read_32(uintptr_t addr);
void mmio_write_32(uintptr_t addr, uint32_t value);
""",
    "lib/psci/psci.h": """#define PSCI_E_SUCCESS 0
#define PSCI_E_INVALID_PARAMS -2
#define PSCI_E_INTERN_FAIL -6
""",
    "lib/smccc.h": """#include <stdint.h>
#ifndef SHIM_SMCCC_H
#define SHIM_SMCCC_H
typedef uintptr_t u_register_t;
#define SMC_32 0
#define SMC_64 1
#define GET_SMC_CC(fid) (((fid) >> 30) & 1U)
#define SMC_UNK -1
int is_caller_non_secure(u_register_t flags);
struct smc_return { uint64_t x[8]; unsigned int n; };
extern struct smc_return smc_return;
#define SMC_RET0(_h) do { (void)(_h); smc_return.n = 0; return 0; } while (0)
#define SMC_RET1(_h, _a) do { (void)(_h); smc_return.n = 1; \\
    smc_return.x[0] = (uint64_t)(_a); return 0; } while (0)
#define SMC_RET2(_h, _a, _b) do { (void)(_h); smc_return.n = 2; \\
    smc_return.x[0] = (uint64_t)(_a); smc_return.x[1] = (uint64_t)(_b); \\
    return 0; } while (0)
#define SMC_RET4(_h, _a, _b, _c, _d) do { (void)(_h); smc_return.n = 4; \\
    smc_return.x[0] = (uint64_t)(_a); smc_return.x[1] = (uint64_t)(_b); \\
    smc_return.x[2] = (uint64_t)(_c); smc_return.x[3] = (uint64_t)(_d); \\
    return 0; } while (0)
static inline uint32_t smc_uuid_word(uint8_t b0, uint8_t b1, uint8_t b2,
                                     uint8_t b3)
{
    return ((uint32_t)b0) | (((uint32_t)b1) << 8) |
           (((uint32_t)b2) << 16) | (((uint32_t)b3) << 24);
}
#define SMC_UUID_RET(_h, _uuid) \\
    SMC_RET4(_h, \\
        smc_uuid_word((_uuid).time_low[0], (_uuid).time_low[1], \\
                      (_uuid).time_low[2], (_uuid).time_low[3]), \\
        smc_uuid_word((_uuid).time_mid[0], (_uuid).time_mid[1], \\
                      (_uuid).time_hi_and_version[0], \\
                      (_uuid).time_hi_and_version[1]), \\
        smc_uuid_word((_uuid).clock_seq_hi_and_reserved, \\
                      (_uuid).clock_seq_low, (_uuid).node[0], \\
                      (_uuid).node[1]), \\
        smc_uuid_word((_uuid).node[2], (_uuid).node[3], \\
                      (_uuid).node[4], (_uuid).node[5]))
#endif /* SHIM_SMCCC_H */
""",
    "lib/utils_def.h": """#define U(x) x##U
#define UL(x) x##UL
#define BIT_32(x) (1U << (x))
""",
    "tools_share/uuid.h": """#include <stdint.h>
#define _UUID_NODE_LEN 6
struct uuid {
    uint8_t time_low[4];
    uint8_t time_mid[2];
    uint8_t time_hi_and_version[2];
    uint8_t clock_seq_hi_and_reserved;
    uint8_t clock_seq_low;
    uint8_t node[_UUID_NODE_LEN];
};
typedef struct uuid uuid_t;
#define DEFINE_SVC_UUID2(_name, _tl, _tm, _th, _cl, _ch, \\
        _n0, _n1, _n2, _n3, _n4, _n5) \\
    static const uuid_t _name = { \\
        {((_tl) >> 24) & 0xFF, ((_tl) >> 16) & 0xFF, \\
         ((_tl) >> 8) & 0xFF, ((_tl) & 0xFF)}, \\
        {((_tm) >> 8) & 0xFF, ((_tm) & 0xFF)}, \\
        {((_th) >> 8) & 0xFF, ((_th) & 0xFF)}, \\
        (_cl), (_ch), { (_n0), (_n1), (_n2), (_n3), (_n4), (_n5) } }
""",
    "mtk_sip_svc.h": """#include <stdint.h>
#define SIP_SVC_CALL_COUNT U(0x8200ff00)
#define SIP_SVC_UID U(0x8200ff01)
#define SIP_SVC_VERSION U(0x8200ff03)
#define MTK_SIP_SVC_VERSION_MAJOR U(0x0)
#define MTK_SIP_SVC_VERSION_MINOR U(0x1)
enum {
    MTK_SIP_E_SUCCESS = 0,
    MTK_SIP_E_INVALID_PARAM = -1,
    MTK_SIP_E_INVALID_RANGE = -3,
    MTK_SIP_E_PERMISSION_DENY = -4,
};
""",
    "mtk_plat_common.h": """/* shim for the platform common header: only the used declaration */
void bl31_prepare_kernel_entry(uint64_t k32_64);
""",
    "plat_private.h": """#include <stdbool.h>
#include "lib/smccc.h"
int mt6797_wdt_capture_control(u_register_t command, u_register_t mask);
int mt6797_toprgu_update(unsigned int id, bool assert);
int mt6797_kernel_power_prepare(void);
bool mt6797_kernel_power_ready(void);
int mt6797_idvfs_set_big_sram_ldo(unsigned int uv);
""",
    "platform_def.h": """#define MT6797_MP0_AXI_CONFIG 0x1022002cUL
""",
}

STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int power_prepare_result;
static bool power_ready_result = true;

struct smc_return smc_return;
static int dsb_count;
void record_dsb(void) { dsb_count++; }

static int caller_non_secure = 1;
int is_caller_non_secure(u_register_t flags)
{
    (void)flags;
    return caller_non_secure;
}

/* simulated OCP/MMIO window */
static unsigned char ocp_mem[0x4000];
static struct { uintptr_t addr; uint32_t value; } writes[64];
static unsigned int write_count;

uint32_t mmio_read_32(uintptr_t addr)
{
    assert(addr % 4 == 0);
    uint32_t value;
    memcpy(&value, ocp_mem + (addr - 0x10220000u), 4);
    return value;
}
void mmio_write_32(uintptr_t addr, uint32_t value)
{
    assert(addr % 4 == 0);
    assert(addr >= 0x10220000u && addr < 0x10224000u);
    assert(write_count < 64);
    writes[write_count++] = (typeof(writes[0])){ addr, value };
    memcpy(ocp_mem + (addr - 0x10220000u), &value, 4);
}

/* platform services */
static int capture_calls, toprgu_calls, emimpu_calls, idvfs_calls;
static int capture_ret, toprgu_ret, emimpu_ret, idvfs_ret;
static u_register_t capture_cmd, capture_mask;
static unsigned int toprgu_id, toprgu_assert;
static uint64_t emimpu_start, emimpu_end;
static uint32_t emimpu_perm, emimpu_read_ret = 0x12345678;
static unsigned int idvfs_arg, sram_ldo_arg;

int mt6797_wdt_capture_control(u_register_t command, u_register_t mask)
{
    capture_cmd = command; capture_mask = mask; capture_calls++;
    return capture_ret;
}
int mt6797_toprgu_update(unsigned int id, bool assert)
{
    toprgu_id = id; toprgu_assert = assert ? 1u : 0u; toprgu_calls++;
    return toprgu_ret;
}
int mt6797_kernel_power_prepare(void) { return power_prepare_result; }
bool mt6797_kernel_power_ready(void) { return power_ready_result; }
uint32_t mt6797_emi_mpu_read(uint32_t offset) { (void)offset; emimpu_calls++; return emimpu_read_ret; }
int mt6797_emi_mpu_set_region(uint64_t start, uint64_t end, uint32_t perm)
{
    emimpu_start = start; emimpu_end = end; emimpu_perm = perm; emimpu_calls++;
    return emimpu_ret;
}
int mt6797_idvfs_set_big_pll(unsigned int mhz) { idvfs_arg = mhz; idvfs_calls++; return idvfs_ret; }
int mt6797_idvfs_set_big_sram_ldo(unsigned int uv) { sram_ldo_arg = uv; idvfs_calls++; return idvfs_ret; }

static uint64_t kernel_entry_arg;
static unsigned int kernel_entry_calls;
void bl31_prepare_kernel_entry(uint64_t k32_64)
{
    kernel_entry_arg = k32_64; kernel_entry_calls++;
}

extern const struct rt_svc_desc rt_svc_desc_mt6797_sip_svc;
uint64_t get_kernel_info_pc(void);
uint64_t get_kernel_info_r0(void);
uint64_t get_kernel_info_r1(void);
uint64_t get_kernel_info_r2(void);
#define SIP rt_svc_desc_mt6797_sip_svc.handler

/* MT6797_SIP_* IDs, from include/plat_sip_calls.h (pinned here for clarity) */
#define SIP_KERNEL_BOOT_32   0x82000200u
#define SIP_KERNEL_BOOT_64   0xc2000200u
#define SIP_MCUSYS_W_32      0x82000201u
#define SIP_MCUSYS_W_64      0xc2000201u
#define SIP_OCP_W_32         0x8200035eu
#define SIP_OCP_W_64         0xc200035eu
#define SIP_OCP_R_32         0x8200035fu
#define SIP_OCP_R_64         0xc200035fu
#define SIP_EMIMPU_R_64      0xc2000208u
#define SIP_EMIMPU_S_64      0xc2000209u
#define SIP_IDVFS_PLL_32     0x820003b8u
#define SIP_IDVFS_LDO_64     0xc20003bfu
#define SIP_WDT_CAP_64       0xc200067au
#define SIP_TOPRGU_64        0xc200067bu
#define SIP_SLEEP_CAPS_64    0xc2000679u
#define SIP_BOOT_CAPS_64     0xc200067cu
#define SIP_CALL_COUNT       0x8200ff00u
#define SIP_UID              0x8200ff01u
#define SIP_VERSION          0x8200ff03u

static uintptr_t call(uint32_t fid, u_register_t x1, u_register_t x2,
                      u_register_t x3, u_register_t x4)
{
    smc_return.n = 0;
    return SIP(fid, x1, x2, x3, x4, NULL, (void *)&smc_return, 1u);
}

static void reset_stubs(void)
{
    caller_non_secure = 1;
    dsb_count = 0; write_count = 0;
    capture_calls = toprgu_calls = emimpu_calls = idvfs_calls = 0;
    capture_ret = toprgu_ret = emimpu_ret = idvfs_ret = 0;
    kernel_entry_calls = 0;
    power_prepare_result = 0;
    power_ready_result = true;
}
"""

MAIN = r"""
int main(void)
{
    /* Secure-world callers are rejected before any dispatch. */
    reset_stubs();
    caller_non_secure = 0;
    assert(call(SIP_OCP_R_32, 0x10220000u, 0, 0, 0) == 0);
    assert(smc_return.n == 1 && smc_return.x[0] == (uint64_t)-4);
    assert(capture_calls == 0 && write_count == 0);

    /* OCP read: in-range aligned address returns the MMIO value. */
    reset_stubs();
    mmio_write_32(0x10220040u, 0xcafebabeu);   /* direct model write */
    write_count = 0;
    assert(call(SIP_OCP_R_64, 0x10220040u, 0, 0, 0) == 0);
    assert(smc_return.n == 1 && smc_return.x[0] == 0xcafebabeu);
    assert(write_count == 0);

    /* OCP read: out-of-range and unaligned addresses are rejected. */
    assert(call(SIP_OCP_R_64, 0x10224000u, 0, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-3);
    assert(call(SIP_OCP_R_64, 0x10220002u, 0, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-3);
    assert(call(SIP_OCP_R_64, 0x10224004u, 0, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-3);

    /* OCP write AArch64: full value stored, no barrier on this path. */
    reset_stubs();
    assert(call(SIP_OCP_W_64, 0x10220100u, 0x11223344u, 0, 0) == 0);
    assert(smc_return.n == 1 && smc_return.x[0] == 0);
    assert(write_count == 1 && writes[0].addr == 0x10220100u &&
           writes[0].value == 0x11223344u);
    assert(dsb_count == 0);

    /* OCP write AArch32: address and value truncated to 32 bits. */
    reset_stubs();
    assert(call(SIP_OCP_W_32, 0x110220100u, 0x1a2b3c4d5u, 0, 0) == 0);
    assert(write_count == 1 && writes[0].addr == 0x10220100u &&
           writes[0].value == 0xa2b3c4d5u);

    /* MCUSYS write: only MP0 AXI ACINACTM, aligned, with dsb. */
    reset_stubs();
    assert(call(SIP_MCUSYS_W_64, 0x1022002cu, 1u, 0, 0) == 0);
    assert(smc_return.x[0] == 0 && write_count == 1 && dsb_count == 1);
    assert(writes[0].addr == 0x1022002cu && writes[0].value == 1u);
    reset_stubs();
    assert(call(SIP_MCUSYS_W_64, 0x1022002du, 1u, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-3 && write_count == 0);
    assert(call(SIP_MCUSYS_W_64, 0x10220030u, 1u, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-3 && write_count == 0);

    /* Kernel boot, AArch64 ABI: registers captured 1:1, entry prepared. */
    reset_stubs();
    assert(call(SIP_KERNEL_BOOT_64, 0x48080000u, 0x40000000u, 0x55u, 1u) == 0);
    assert(smc_return.n == 0 && kernel_entry_calls == 1 && kernel_entry_arg == 1u);
    assert(get_kernel_info_pc() == 0x48080000u);
    assert(get_kernel_info_r0() == 0x40000000u && get_kernel_info_r1() == 0x55u);
    assert(get_kernel_info_r2() == 0u);

    /* Kernel boot, AArch32 ABI: r0 forced 0, machtype/tags shifted. */
    reset_stubs();
    assert(call(SIP_KERNEL_BOOT_32, 0x48080000u, 0x1234u, 0x40000100u, 0u) == 0);
    assert(smc_return.n == 0 && kernel_entry_arg == 0u);
    assert(get_kernel_info_r0() == 0u && get_kernel_info_r1() == 0x1234u);
    assert(get_kernel_info_r2() == 0x40000100u);

    /* Kernel boot with failed power preparation: intern failure, no entry. */
    reset_stubs();
    power_prepare_result = -6;
    assert(call(SIP_KERNEL_BOOT_64, 0x48080000u, 0, 0, 1u) == 0);
    assert(smc_return.n == 1 && smc_return.x[0] == (uint64_t)-6);
    assert(kernel_entry_calls == 0);

    /* TOPRGU update: id and assert flag validation. */
    reset_stubs();
    assert(call(SIP_TOPRGU_64, 17u, 1u, 0, 0) == 0);
    assert(smc_return.x[0] == 0 && toprgu_calls == 1 && toprgu_id == 17u &&
           toprgu_assert == 1u);
    assert(call(SIP_TOPRGU_64, 18u, 1u, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-2 && toprgu_calls == 1);
    assert(call(SIP_TOPRGU_64, 2u, 2u, 0, 0) == 0);
    assert(smc_return.x[0] == (uint64_t)-2 && toprgu_calls == 1);

    /* WDT capture control is forwarded. */
    reset_stubs();
    capture_ret = 7;
    assert(call(SIP_WDT_CAP_64, 1u, 0x3fu, 0, 0) == 0);
    assert(smc_return.x[0] == 7 && capture_calls == 1 &&
           capture_cmd == 1u && capture_mask == 0x3fu);

    /* Capability and standard queries. */
    reset_stubs();
    assert(call(SIP_SLEEP_CAPS_64, 0, 0, 0, 0) == 0);
    assert(smc_return.n == 2 && smc_return.x[0] == 0x01010001u &&
           smc_return.x[1] == 1u);
    reset_stubs();
    power_ready_result = false;
    assert(call(SIP_BOOT_CAPS_64, 0, 0, 0, 0) == 0);
    assert(smc_return.n == 4 && smc_return.x[0] == 0x58323031u &&
           smc_return.x[1] == 1u && smc_return.x[2] == 3u &&
           smc_return.x[3] == 0u);
    reset_stubs();
    assert(call(SIP_CALL_COUNT, 0, 0, 0, 0) == 0);
    assert(smc_return.n == 1 && smc_return.x[0] == 12u);
    assert(call(SIP_VERSION, 0, 0, 0, 0) == 0);
    assert(smc_return.n == 2 && smc_return.x[0] == 0u && smc_return.x[1] == 1u);

    /* Vendor UUID (a42b58f7-6242-7d4d-80e5-8f9505000f3d). */
    assert(call(SIP_UID, 0, 0, 0, 0) == 0);
    /* SMCCC 5.3: the UUID words carry the struct bytes little-endian. */
    assert(smc_return.n == 4 && smc_return.x[0] == 0xf7582ba4u &&
           smc_return.x[1] == 0x4d7d4262u && smc_return.x[2] == 0x958fe580u &&
           smc_return.x[3] == 0x3d0f0005u);

    /* EMIMPU and IDVFS forwarding with AArch32 truncation where relevant. */
    reset_stubs();
    assert(call(SIP_EMIMPU_R_64, 0x20u, 0, 0, 0) == 0);
    assert(smc_return.x[0] == 0x12345678u && emimpu_calls == 1);
    assert(call(SIP_EMIMPU_S_64, 0x40000000u, 0x40100000u, 3u, 0) == 0);
    assert(emimpu_start == 0x40000000u && emimpu_end == 0x40100000u &&
           emimpu_perm == 3u);
    assert(call(SIP_IDVFS_PLL_32, 1100000u, 0, 0, 0) == 0);
    assert(idvfs_calls == 1 && idvfs_arg == 1100000u);
    assert(call(SIP_IDVFS_LDO_64, 70000u, 0, 0, 0) == 0);
    assert(idvfs_calls == 2 && sram_ldo_arg == 70000u);

    /* Unknown SMC: SMC_UNK. */
    reset_stubs();
    assert(call(0x8200deadu, 0, 0, 0, 0) == 0);
    assert(smc_return.n == 1 && smc_return.x[0] == (uint64_t)-1);

    puts("Real plat_sip_calls.c: secure rejection, 32/64-bit truncation,"
         " OCP/MCUSYS ranges, boot ABI capture, caps/UUID: PASS");
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="mt6797-sip-") as temp:
        folder = Path(temp)
        shim = folder / "shim"
        shim.mkdir()
        for name, text in SHIM_FILES.items():
            path = shim / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(text)
        unit = folder / "plat_sip_calls.c"
        unit.write_text(UNIT.read_text())
        harness = folder / "harness.c"
        harness.write_text("#include <stdint.h>\n"
                           "#include <lib/smccc.h>\n"
                           "#include <common/runtime_svc.h>\n"
                           + STUBS + "\n" + MAIN)
        binary = folder / "test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra",
                        "-Wno-unused-parameter", "-Werror", "-DX20_WDT_FIQ=1",
                        "-I", str(shim), "-I", str(shim / "lib"),
                        "-I", str(shim / "common"), "-I", str(shim / "bl31"),
                        "-I", str(shim / "tools_share"),
                        "-I", str(PLAT_INCLUDE), "-I", str(MTK_INCLUDE),
                        "-I", str(UNIT.parent),
                        str(unit), str(harness), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
