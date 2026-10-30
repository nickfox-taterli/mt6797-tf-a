#!/usr/bin/env python3
"""Exercise real MT6797 CPU power code and PSCI peer completion on host MMIO."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def without_includes(path):
    return re.sub(r'^#include[^\n]*\n', '', path.read_text(), flags=re.M)


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define U(x) x##U
#define UL(x) x##UL
#define BIT_32(x) (1U << (x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define NOTICE(...) ((void)0)
#define ERROR(...) ((void)0)
#define PSCI_E_SUCCESS 0
#define PSCI_E_INVALID_PARAMS -2
#define PSCI_E_DENIED -3
#define PSCI_E_INTERN_FAIL -6
typedef uintptr_t u_register_t;
typedef int spinlock_t;
static unsigned int caller;
static unsigned int reads, writes, spm_writes, delayed;
static uint32_t fail_protect;
static int lock_depth;
static void spin_lock(spinlock_t *lock) {
    assert(!*lock); *lock = 1; lock_depth++;
}
static void spin_unlock(spinlock_t *lock) {
    assert(*lock); *lock = 0; lock_depth--;
}
static void dsb(void) {}
static void udelay(unsigned int us) { delayed += us; }
static unsigned int plat_my_core_pos(void) { return caller; }
static int plat_core_pos_by_mpidr(u_register_t mpidr) {
    unsigned int cl = (mpidr >> 8) & 0xff, core = mpidr & 0xff;
    return (cl < 3 && core < (cl == 2 ? 2 : 4)) ? (int)(cl * 4 + core) : -1;
}
static void bl31_warm_entrypoint(void) {}
struct reg { uintptr_t addr; uint32_t value; };
static struct reg regs[512];
static unsigned int reg_count;
static uint32_t *regp(uintptr_t addr) {
    /* The coherent pending array is real host memory, not an SoC register. */
    if (addr > UINT32_MAX) return (uint32_t *)addr;
    for (unsigned int i=0; i<reg_count; i++)
        if (regs[i].addr == addr) return &regs[i].value;
    assert(reg_count < ARRAY_SIZE(regs));
    regs[reg_count] = (struct reg){addr, 0};
    return &regs[reg_count++].value;
}
static uint32_t mmio_read_32(uintptr_t addr) { reads++; return *regp(addr); }
static void mmio_write_32(uintptr_t addr, uint32_t value);
'''

MMIO = r'''
static void mmio_write_32(uintptr_t addr, uint32_t value) {
    writes++;
    if (addr >= SPM_BASE && addr < SPM_BASE + 0x1000) spm_writes++;
    *regp(addr) = value;
    if (addr == MT6797_INFRA_PROTECTEN1)
        *regp(MT6797_INFRA_PROTECTSTA3) = value & ~fail_protect;
    if (addr >= SPM_BASE+0x220 && addr <= SPM_BASE+0x23c) {
        unsigned int cpu = (addr-SPM_BASE-0x220)/4;
        uint32_t mask = BIT_32(15-cpu);
        if (value & BIT_32(8)) *regp(addr) |= BIT_32(12);
        else *regp(addr) &= ~BIT_32(12);
        for (unsigned int i=0; i<2; i++) {
            uintptr_t status = i ? SPM_CPU_PWR_STATUS_2ND : SPM_CPU_PWR_STATUS;
            if (value & BIT_32(2)) *regp(status) |= mask;
            else *regp(status) &= ~mask;
        }
    }
    if (addr == SPM_MP0_CPUSYS_PWR_CON || addr == SPM_MP1_CPUSYS_PWR_CON) {
        if (value & BIT_32(8)) *regp(addr) |= BIT_32(12);
        else *regp(addr) &= ~BIT_32(12);
    }
    if (addr == SPM_MP2_CPU0_PWR_CON || addr == SPM_MP2_CPU0_PWR_CON+4) {
        unsigned int cpu = 8 + (addr-SPM_MP2_CPU0_PWR_CON)/4;
        uint32_t mask = BIT_32(15-cpu);
        uintptr_t spmc = cpu == 8 ? MT6797_BIG_CPU0_SPMC : MT6797_BIG_CPU1_SPMC;
        if (value & BIT_32(2)) {
            *regp(SPM_CPU_PWR_STATUS) |= mask;
            *regp(spmc) |= BIT_32(17);
        } else {
            *regp(SPM_CPU_PWR_STATUS) &= ~mask;
            *regp(spmc) &= ~BIT_32(17);
        }
    }
    if (addr == SPM_MP2_CPUSYS_PWR_CON) {
        if (value & BIT_32(2)) *regp(SPM_CPU_PWR_STATUS) |= BIT_32(17);
        else *regp(SPM_CPU_PWR_STATUS) &= ~BIT_32(17);
    }
}
'''

POWER_TEST = r'''
static void setup(void) {
    memset(regs, 0, sizeof(regs)); reg_count=0;
    memset(off_pending, 0, sizeof(off_pending));
    caller=0; little_on=0xff; big_on=3; big_cluster_on=true;
    reads=writes=spm_writes=delayed=fail_protect=0; power_lock=0; lock_depth=0;
    *regp(SPM_CPU_PWR_STATUS)=0xffc0 | BIT_32(17);
    *regp(SPM_CPU_PWR_STATUS_2ND)=0xffc0;
    *regp(MT6797_MP0_CA7L_MISC_CONFIG)=0xf000000;
    *regp(MT6797_MP1_CA7L_MISC_CONFIG)=0xf000000;
    *regp(SPM_CPU_IDLE_STA)=BIT_32(20)|BIT_32(21);
    *regp(SPM_CPU_IDLE_STA2)=BIT_32(10)|BIT_32(11)|BIT_32(20);
    *regp(MT6797_BIG_HW_SEM)=1;
    *regp(MT6797_BIG_ARMPLL_CON0)=1;
    *regp(SPM_MP2_CPUSYS_PWR_CON)=5;
    for (unsigned int cpu=0; cpu<10; cpu++)
        *regp(cpu < 8 ? SPM_BASE+0x220+cpu*4 : SPM_MP2_CPU0_PWR_CON+(cpu-8)*4)=0x1000d;
}
static u_register_t mpidr(unsigned int cpu) { return (cpu/4)*0x100+cpu%4; }
int main(void) {
    for (unsigned int cpu=1; cpu<10; cpu++) {
        setup();
        assert(mt6797_power_off_finish(mpidr(cpu)) == 0 && writes == 0);
        caller=cpu; mt6797_power_off_prepare(cpu);
        assert(mt6797_power_off_finish(mpidr(cpu)) == PSCI_E_DENIED);
        caller=0;
        assert(mt6797_power_off_finish(mpidr(cpu)) == 0);
        assert(!off_pending[cpu]);
        assert(!(*regp(SPM_CPU_PWR_STATUS)&BIT_32(15-cpu)));
        assert(cpu < 8 ? !(little_on&BIT_32(cpu)) : !(big_on&BIT_32(cpu-8)));
        unsigned int count=writes;
        assert(mt6797_power_off_finish(mpidr(cpu)) == 0 && writes == count);
        assert(mt6797_power_on_core(cpu) == 0);
        assert(*regp(SPM_CPU_PWR_STATUS)&BIT_32(15-cpu));
        assert(!power_lock && lock_depth == 0);
    }
    setup(); little_on=BIT_32(0)|BIT_32(4);
    mt6797_power_off_prepare(4);
    assert(mt6797_power_off_finish(0x100) == 0);
    assert((*regp(MT6797_INFRA_PROTECTSTA3)&0x222)==0x222);
    assert(!(*regp(SPM_MP1_CPUSYS_PWR_CON)&(BIT_32(2)|BIT_32(3))));
    setup();
    mt6797_power_off_prepare(8); assert(mt6797_power_off_finish(0x200)==0);
    mt6797_power_off_prepare(9); assert(mt6797_power_off_finish(0x201)==0);
    assert(big_on==0 && !big_cluster_on);
    assert(!(*regp(MT6797_BIG_ARMPLL_CON0)&1));
    assert(*regp(SPM_CPU_EXT_BUCK_ISO)&2);
    assert((*regp(MT6797_INFRA_PROTECTSTA3)&0x444)==0x444);
    setup(); *regp(MT6797_MP0_CA7L_MISC_CONFIG)=0;
    mt6797_power_off_prepare(1);
    assert(mt6797_power_off_finish(1)==PSCI_E_INTERN_FAIL);
    assert(spm_writes==0 && off_pending[1] && (little_on&2));
    assert(reads<100100 && !power_lock);
    setup(); little_on=BIT_32(0)|BIT_32(4); fail_protect=BIT_32(9);
    mt6797_power_off_prepare(4);
    assert(mt6797_power_off_finish(0x100)==PSCI_E_INTERN_FAIL);
    assert(off_pending[4]); /* Partial protection ACK cannot mean fully OFF. */
    assert(!power_lock && lock_depth==0);
    puts("CPU1-9 physical off/on, peer-only/idempotence, last-cluster off, WFI/partial-ACK failures: PASS");
}
'''

AFFINITY_TEST = r'''
enum {AFF_STATE_ON=0, AFF_STATE_OFF=1, AFF_STATE_ON_PENDING=2, PSCI_CPU_PWR_LVL=0};
struct node { spinlock_t cpu_lock; } node;
static int state, completions, result, race;
static int finish(u_register_t cpu) {
    (void)cpu; assert(node.cpu_lock); completions++; return result;
}
struct ops { int (*pwr_domain_off_finish)(u_register_t); } ops;
static struct ops *psci_plat_pm_ops=&ops;
#define PER_CPU_BY_INDEX(table,idx) (&node)
#define flush_cpu_data_by_index(idx,data) ((void)(idx))
static bool is_valid_mpidr(u_register_t cpu) { return cpu<10; }
static int psci_get_aff_info_state_by_idx(unsigned int idx) {
    (void)idx;
    /* Simulate CPU_ON changing logical state before the lock is acquired. */
    if (race && node.cpu_lock) { state=AFF_STATE_ON_PENDING; race=0; }
    return state;
}
'''

AFFINITY_MAIN = r'''
int main(void) {
    caller=0; ops.pwr_domain_off_finish=finish;
    assert(psci_affinity_info(10,0)==PSCI_E_INVALID_PARAMS);
    assert(psci_affinity_info(1,1)==PSCI_E_INVALID_PARAMS);
    state=AFF_STATE_ON; assert(psci_affinity_info(1,0)==AFF_STATE_ON && !completions);
    state=AFF_STATE_ON_PENDING;
    assert(psci_affinity_info(1,0)==AFF_STATE_ON_PENDING && !completions);
    state=AFF_STATE_OFF; result=PSCI_E_INTERN_FAIL;
    assert(psci_affinity_info(1,0)==AFF_STATE_ON && completions==1);
    result=0; assert(psci_affinity_info(1,0)==AFF_STATE_OFF && completions==2);
    race=1; state=AFF_STATE_OFF;
    assert(psci_affinity_info(1,0)==AFF_STATE_ON_PENDING && completions==2);
    ops.pwr_domain_off_finish=NULL; state=AFF_STATE_OFF;
    assert(psci_affinity_info(1,0)==AFF_STATE_OFF && completions==2);
    assert(!node.cpu_lock && lock_depth==0);
    puts("Real AFFINITY_INFO validation, CPU_ON serialization, completion failure and other-platform fallback: PASS");
}
'''


def main():
    platform = ROOT / 'plat/mediatek/mt6797'
    power = COMMON + without_includes(platform / 'include/platform_def.h') + MMIO
    power += without_includes(platform / 'power.c') + POWER_TEST
    main_source = (ROOT / 'lib/psci/psci_main.c').read_text()
    affinity = main_source[main_source.index('int psci_affinity_info('):main_source.index('\nint psci_migrate(')]
    with tempfile.TemporaryDirectory(prefix='mt6797-power-') as temp:
        for name, code in (('power', power), ('affinity', COMMON + AFFINITY_TEST + affinity + AFFINITY_MAIN)):
            source = Path(temp) / (name + '.c')
            binary = Path(temp) / name
            source.write_text(code)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                            '-Wno-unused-variable', str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
