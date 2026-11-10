# SadBoard External

SadBoard 的 Buildroot 实验层，与 `bsp-base-external` 同时加载。主项目位于 [workspace](https://github.com/linux-bsp/workspace) 的 `SadBoard` 分支。

## 当前支持

提供 TI AM6254 调试环境的标准 defconfig 入口、用户态工具配置、内核跟踪配置和目标端检查脚本。板级启动配置、源码版本和基础服务来自 BSP 层。基线使用 systemd，默认 TFTP 内核/DTB＋NFS root；保留本地 RAW A/B 启动与 OTA kernel/rootfs。生成 rootfs.tar、images/netboot/ 和匹配调试资料；工厂 SD 镜像只供新介质初始化。板上 SPL/U-Boot 不由普通实验 OTA 更新。

已提供崩溃、FD 耗尽、OOM、CPU、锁、隔离网络、启动 panic、周期漂移、队列积压和 TCP 批处理十个原有手动案例，并新增 24 个基础场景（再增 20 个进阶以及 CMA/四项启动内核实验，共 59 例）。使用 `sadboard list`、`sadboard run crash`、`sadboard verify crash` 等命令，启动时没有自动故障注入或随机题库调度。可选 KGDB、锁检查和 kmemleak 配置仍需要分别上板验证；不默认启用 KASAN、eBPF/BTF 或 kdump。

`board/am625/uboot-netboot.config` 默认启用独立的 `CONFIG_AM62X_BOOT_TFTP_KERNEL` 和
`CONFIG_AM62X_BOOT_NFS_ROOT`，分别映射到 U-Boot 的 `tftp_kernel`、`nfs_root`。
普通 BSP 两项均关闭。任一网络模式停用生产 OTA/槽位确认，使用独立调试健康门禁和 SBNET1 参考构建记录。NFS 根下的实验结果默认写 `/srv/sadboard`，主机可直接访问；本地根仍写 `/data/sadboard`。SD `/data` 保存设备日志和启动状态。部署用 `scripts/deploy-netboot.sh`，完整四模式配置和操作见工作区教材。

`sadboard results` 显示当前结果目录，`SADBOARD_RESULTS` 可覆盖它。NFS 实验结束后自动开放产物读取权限；手工生成的 perf/core/trace 文件可用 `sadboard share 结果子目录` 开放主机读取。空间检查针对结果所在的实际文件系统。

新增入口：`sadboard verify deadline 200 10000 2000`、`sadboard verify backlog 512 64`、`sadboard verify tcp 8192`。故障与参考版保留相同工作量，记录逐次时间/队列和完整性指标；TCP 只操作自建 namespace。新基础入口例如 `sadboard verify crash-sigbus`、`sadboard verify fd-cloexec`、`sadboard verify sync-abba`、`sadboard verify net-framing`，全部固定有限负载。`sadboard list` 列出完整命令与教材 ID。主项目统一教材还包含 35 个基础、24 个进阶设计（共 59 例）；57 例通过设备命令操作，B-BOOT02/03 由 scripts/sadboard-board.py 经主机串口控制真实 AM625 的 RAM 启动。看门狗探测为锁死实验前置检查，不另算案例；本阶段验收范围见主仓记录。

## 标准 defconfig 构建

初始化 workspace 中的源码子模块、安装 Buildroot 主机依赖后，直接进入 Buildroot：

```sh
cd buildroot
BR2_EXTERNAL="../bsp-base-external ../sadboard-external" \
  make sadboard_am625_debug_defconfig && make all
```

该命令加载已提交的 `configs/sadboard_am625_debug_defconfig`。Buildroot 会记住两个 external，后续 `make all` 无需再次指定。默认独立构建工具链，不依赖另一份工作区；不带 `O=` 时配置在 `buildroot/.config`，产物在 `buildroot/output/images/`。

需要独立输出目录时，从 workspace 根目录执行：

```sh
make -C buildroot O="$PWD/output/am625-native-debug" \
  BR2_EXTERNAL="$PWD/bsp-base-external $PWD/sadboard-external" \
  sadboard_am625_debug_defconfig
make -C output/am625-native-debug all
```

切换工具链时必须使用新的输出目录，不能覆盖已有外部工具链构建的配置后直接增量编译。可以使用 `make list-defconfigs` 查看入口。

## 原生配置与标准配置维护

工具链、下载目录和额外内核片段统一通过 Buildroot 的 `menuconfig`、`olddefconfig` 和 `savedefconfig` 管理。
使用已有 SDK 时，在新的输出目录选择 External toolchain / Custom / Pre-installed，设置实际 SDK 路径、
`aarch64-buildroot-linux-gnu` 前缀、GCC 15、glibc、Linux 7.0 headers 与 C++ 支持。
共享下载缓存使用 Build options 中的 Download dir；不要把本机绝对路径写进共享 defconfig。

维护标准配置时从 workspace 根目录执行：

```sh
make -C output/am625-native-debug menuconfig
make -C output/am625-native-debug savedefconfig \
  DEFCONFIG="$PWD/sadboard-external/configs/sadboard_am625_debug_defconfig"
git -C sadboard-external diff -- configs/sadboard_am625_debug_defconfig
```

标准 defconfig 是本层唯一的 Buildroot 配置入口。Linux、BusyBox 与 U-Boot 仍分别使用原生的
配置文件/片段选项，不再额外合成另一份 Buildroot 配置。普通 BSP 基线独立维护。

完整操作说明与实际验证记录见 [调试教材](https://github.com/linux-bsp/workspace/blob/SadBoard/docs/obsidian/sadboard/00-实验准备与通用方法/02-TFTP-NFS%20调试启动与编译开关.md)。

## 目录与职责

- `configs/sadboard_am625_debug_defconfig`：Buildroot 标准构建入口。
- `kernel/`：基础跟踪与可选诊断片段。
- `rootfs-overlay/`：systemd 准备 debugfs/tracefs；`sadboard-debug-check` 只检查环境。
- `package/sadboard-labs/`：手动案例、参考修复和命令入口。
- `scripts/export-debug.sh`：在 BSP 打包完成后保存匹配符号、源码与 SHA256。
- `tests/test-labs.py`：故障触发、参考修复及入口边界回归。
- `scripts/verify-debug.sh`：部署前核对固件/符号对应关系、完整性与内核能力。
- `scripts/netboot-build.sh`：原生 post-build 调试身份记录。
- `scripts/build-boot-labs.sh`：打包 RAM 启动实验；ELF 由实验包 Makefile 用目标编译器构建。
- `scripts/deploy-netboot.sh`：主机 TFTP/NFS 部署。
- `scripts/sadboard-board.py`：可选的主机串口验收工具，不参与构建或设备运行。
- `Config.in`、`external.mk`：实验软件包入口。

基础层名称保持 `LINUX_BSP`，本层名称为 `SADBOARD`。标准 defconfig 已包含调试配置；加载两个 external 用于解析板级路径、配置和软件包。文件中的路径通过 `BR2_EXTERNAL_*_PATH` 引用，不写死个人目录。保留外部材料的原始许可与来源；项目代码许可尚未确定。

## 调试资料与验证

通用目标文件开启剥离以满足 128 MiB 槽位约束，`usr/libexec/sadboard` 除外。内核与程序符号、源码、配置和固件 ID 自动保存到 `images/debug/`；远程 GDB 使用同次构建的 `staging/`。参考修复检查不等同于自动评判任意用户补丁。

```sh
python3 sadboard-external/tests/test-labs.py
python3 sadboard-external/tests/test-netboot.py
python3 sadboard-external/tests/test-results.py
```

本地实验包构建强制使用目标编译器重编译，避免把源码目录中残留的主机程序带入固件。验证脚本同时检查 AArch64 ELF64 架构、调试段和 SHA256；有符号不代表架构正确。测试脚本在临时目录原生构建。

配置与构建成功、真实 OTA success、工具和场景实机通过分别记录，不混作一个结论。

设备不安装 Python 解释器；Device Manager 与设备工具均为原生 C 程序。自定义构建使用 Buildroot 包、Bash post-build/post-image、Kconfig/CMake 和原生主机工具。上游 U-Boot/binman、OP-TEE 等由 Buildroot 管理的主机 Python 依赖保留；本仓 Python 测试和串口控制器仅在开发主机运行。
