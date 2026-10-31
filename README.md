# SadBoard External

SadBoard 的 Buildroot 实验层，与 `bsp-base-external` 同时加载。主项目位于 [workspace](https://github.com/linux-bsp/workspace) 的 `SadBoard` 分支。

## 当前支持

提供 TI AM6254 调试环境的配置生成入口、用户态工具配置、内核跟踪配置和目标端检查脚本。板级启动配置、源码版本和基础服务来自 BSP 层。调试环境优先复用板上现有启动链，生成内核与 NFS 根文件系统；不生成产品 RAW A/B SD 卡镜像。

目前没有自动故障注入、题库调度或验收服务。可选 KGDB、锁检查和 kmemleak 配置仍需要分别上板验证；不默认启用 KASAN、eBPF/BTF 或 kdump。

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

## 可选的配置生成脚本

只有需要本机工具链、共享下载缓存或可选诊断片段时，才需要手动运行生成脚本：

```sh
python3 sadboard-external/scripts/configure-am625.py --output output/am625-custom-debug
make -C output/am625-custom-debug all
```

可选参数：

- `--output PATH`：独立输出目录；已有 `.config` 时拒绝覆盖。
- `--dl-dir PATH`：复用下载缓存。
- `--toolchain PATH`：使用已有的 AArch64 GCC 15/glibc 工具链，前缀为 `aarch64-buildroot-linux-gnu`，Linux headers 为 7.0。
- `--kernel-extra kgdb` / `--kernel-extra memory`：追加诊断片段。
- `--export-defconfig`：维护者重新生成标准 defconfig；不能与本机选项或可选诊断片段组合。

## 维护标准配置

标准 defconfig 是 BSP 基线与本层两个片段的生成快照。基础配置更新后，维护者在 workspace 根目录重新生成并检查差异：

```sh
regen_dir=$(mktemp -d)
python3 sadboard-external/scripts/configure-am625.py \
  --output "$regen_dir" --export-defconfig
git -C sadboard-external diff -- configs/sadboard_am625_debug_defconfig
```

脚本调用 Buildroot 的 `olddefconfig` 和 `savedefconfig`，导出可直接加载的最小配置。将生成文件与对应输入改动一起提交。普通使用者无需执行此步骤。

完整操作说明与实际验证记录见 [AM6254 环境指南](https://github.com/linux-bsp/workspace/blob/SadBoard/docs/am625-bootlin-debug-environment.md)。

## 目录与职责

- `configs/sadboard_am625_debug_defconfig`：Buildroot 标准构建入口。
- `scripts/configure-am625.py`：可选配置生成、依赖检查与标准配置导出。
- `configs/debug-tools.config`：GDB、strace、Valgrind、perf、trace-cmd、网络工具等。
- `board/am625/debug.config`：AM6254 的组合配置与 NFS 实验产物策略。
- `kernel/`：基础跟踪与可选诊断片段。
- `rootfs-overlay/`：启动时挂载 debugfs/tracefs；`sadboard-debug-check` 仅检查环境，不触发故障。
- `Config.in`、`external.mk`：后续实验软件包入口。

基础层名称保持 `LINUX_BSP`，本层名称为 `SADBOARD`。标准 defconfig 已包含组合结果；加载两个 external 用于解析板级路径、配置和软件包。文件中的路径通过 `BR2_EXTERNAL_*_PATH` 引用，不写死个人目录。保留外部材料的原始许可与来源；项目代码许可尚未确定。
