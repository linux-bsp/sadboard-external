# SadBoard External

SadBoard 的 Buildroot 实验层，与 `bsp-base-external` 同时加载。主项目位于 [workspace](https://github.com/linux-bsp/workspace) 的 `SadBoard` 分支。

## 当前支持

提供 TI AM6254 调试环境的配置生成入口、用户态工具配置、内核跟踪配置和目标端检查脚本。板级启动配置、源码版本和基础服务来自 BSP 层。调试环境优先复用板上现有启动链，生成内核与 NFS 根文件系统；不生成产品 RAW A/B SD 卡镜像。

目前没有自动故障注入、题库调度或验收服务。可选 KGDB、锁检查和 kmemleak 配置仍需要分别上板验证；不默认启用 KASAN、eBPF/BTF 或 kdump。

## 配置与构建

从 workspace 根目录执行：

```sh
python3 sadboard-external/scripts/configure-am625.py
make -C output/am625-debug
```

脚本将 BSP defconfig 与本层配置合并，调用 Buildroot `olddefconfig`，检查关键调试工具没有被依赖规则禁用。默认独立构建工具链；已有 `.config` 时拒绝覆盖，后续使用 `make -C output/am625-debug` 继续构建。

可选参数：

- `--output PATH`：独立输出目录，建议放在 workspace 的 `output/` 下。
- `--dl-dir PATH`：复用下载缓存。
- `--toolchain PATH`：使用已有的 AArch64 GCC 15/glibc 工具链，前缀为 `aarch64-buildroot-linux-gnu`，Linux headers 为 7.0；Buildroot 构建时进一步检查兼容性。
- `--kernel-extra kgdb` / `--kernel-extra memory`：追加诊断配置；用新的输出目录与基础性能环境分开。

完整操作说明与实际验证记录见 [AM6254 环境指南](https://github.com/linux-bsp/workspace/blob/SadBoard/docs/am625-bootlin-debug-environment.md)。

## 目录与职责

- `scripts/configure-am625.py`：配置生成和依赖检查。
- `configs/debug-tools.config`：GDB、strace、Valgrind、perf、trace-cmd、网络工具等。
- `board/am625/debug.config`：AM6254 的组合配置与 NFS 实验产物策略。
- `kernel/`：基础跟踪与可选诊断片段。
- `rootfs-overlay/`：启动时挂载 debugfs/tracefs；`sadboard-debug-check` 仅检查环境，不触发故障。
- `Config.in`、`external.mk`：后续实验软件包入口。

基础层名称保持 `LINUX_BSP`，本层名称为 `SADBOARD`。两个 external 不会自动合并 defconfig，应通过上述脚本组合。文件中的路径通过 `BR2_EXTERNAL_*_PATH` 引用，不写死个人目录。保留外部材料的原始许可与来源；项目代码许可尚未确定。
