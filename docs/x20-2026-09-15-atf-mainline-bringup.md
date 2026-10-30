# X20 / MT6797 主线 ATF 实机验收

日期：2026-09-15

## 验收基线

主线 Linux 6.18.51 已经可以在真实 X20-96 / MT6797 目标板启动，当前已确认：

- 10 个 CPU 全部启动并 online；
- 三个 cluster 的 OPP/调频路径可用，启动日志能读到 cluster0 `897000kHz`、cluster1 `1274000kHz`、cluster2 `676000kHz`；
- eMMC 可以在较低速度枚举和读写；HS400 尚未通过，不作为本阶段目标；
- 主线内核模块已经安装在项目内 Debian rootfs 中；
- eMMC `system` 分区内容直接来自本项目 Debian rootfs 镜像。

本记录只覆盖主线 ATF 与已验证的主线 kernel/LK 启动组合，不代表 Android 分区已经清理或迁移完成。

## ATF 变更

主线 ATF 仓库：`/root/code/x20/atf-upstream`，分支：`x20/mt6797-atf`。

本阶段以小提交推进：

```text
6f9f59631 plat: mt6797: allow non-secure CCI access
16a17bbaa plat: mt6797: initialize GIC on CPU_ON finish
```

第一笔提交在 CCI-400 初始化后开放非安全访问，保持与旧 Vendor ATF 的行为一致。第二笔提交在 PSCI `CPU_ON` 完成路径初始化 secondary CPU 的 GIC redistributor/interface，解决 Linux GICv3 CPU interface 被高异常级屏蔽后 CPU1 无法上线的问题。

构建命令：

```sh
make -C /root/code/x20/atf-upstream \
  PLAT=mt6797 CROSS_COMPILE=aarch64-linux-gnu- DEBUG=1 LOG_LEVEL=20 \
  RESET_TO_BL31=1 -j2 bl31
```

当前 BL31 原始二进制：

```text
build/mt6797/debug/bl31.bin
SHA-256 d74503fa3821a9e23c4363ac42cdfdbf413eaac77a2775619a313e378853c025
```

用于实机的打包镜像：

```text
/root/code/x20/out/trustzone-mt6797-atf-gic-cpuif-v29.bin
SHA-256 6f597fc0186832af13b6d42a94a102997763d60bdf527b099a2ae39bf3ea3c18
```

## 实机证据

实机串口完整记录：

```text
/root/x20-fes-test/normal-repair/mainline-atf-v29-gic-cpuif-serial.log
```

该镜像通过 FES 临时进入 Fastboot，只刷写 `tee1`，保留同一份主线 boot 镜像和已验证的 headless LK。日志中出现：

```text
MT6797 BL31...
psci: PSCIv1.1 detected in firmware.
smp: Brought up 1 node, 10 CPUs
SMP: Total of 10 processors activated
CPU: All CPU(s) started at EL2
mt6797-cpufreq cluster0 current rate 897000kHz
mt6797-cpufreq cluster1 current rate 1274000kHz
mt6797-cpufreq cluster2 current rate 676000kHz
mmc0 new high speed MMC
mmcblk0 p1 ... p30
```

其中 `CPU1` 至 `CPU9` 均有 secondary processor boot 记录，随后 Linux 进入 initramfs。这里的 `high speed MMC` 是当前低速 eMMC 路径的枚举结果，不应解读为 HS400 已通过。

## 差分结论

- Vendor ATF + 同一主线 kernel/LK：10 核、OPP 和 eMMC 枚举正常。
- 主线 ATF v28（仅 CCI 修复）：CCI 问题消失，但 CPU1 因 GICv3 CPU interface 未启用而触发 capability panic。
- 主线 ATF v29（增加 secondary GIC 初始化）：10 核全部上线，OPP 可见，低速 eMMC 枚举并列出 30 个分区。

当前主线 ATF 与 Vendor ATF 仍有 `SCR_EL3.FIQ=0` 对 `FIQ=1` 的差异；它不是本阶段 10 核/OPP/eMMC 验收的阻塞项，后续若验证 secure interrupt、watchdog 或其他依赖 FIQ 的功能，再单独做变量和提交。

## 未纳入本阶段

- HS400 调速和高速 eMMC 稳定性；
- Android 分区精简、冗余分区清理和最终 GPT 规划；
- CSI、DSI、AUDIO、WLAN/BT 等未接硬件或低优先级外设；
- Debian rootfs 的交互式 `modprobe` 实机闭环。此前尝试进入最小 initramfs 时工具和时序不足，不能作为 rootfs 或模块缺失证据。
