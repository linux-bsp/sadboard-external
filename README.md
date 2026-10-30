# SadBoard External

SadBoard 的 Buildroot 实验扩展层，与提供板级支持的 `bsp-base-external` 配合使用。

## 当前状态

当前仅提供可被 Buildroot 加载的 external 骨架，尚未包含实验软件包、故障场景、内核配置片段或实验 defconfig。

## 分层职责

- `bsp-base-external`：板级配置、启动支持、镜像布局和基础软件包。
- `sadboard-external`：后续接入实验软件包、调试配置和实验 rootfs overlay。
- [SadBoard 主仓库](https://github.com/linux-bsp/BadBoard)：故障场景、运行工具、验收逻辑和学习文档。

## 加载方式

当 Buildroot 和两个 external 位于同一工作区时，可在工作区根目录执行：

```sh
make -C buildroot \
  BR2_EXTERNAL="$PWD/bsp-base-external $PWD/sadboard-external" \
  list-defconfigs
```

此命令用于检查 external 加载和可用配置，不会构建固件。两个 external 一起加载不会自动合并 defconfig 或启用实验工具。

本层名称为 `SADBOARD`，路径变量为 `BR2_EXTERNAL_SADBOARD_PATH`；板级层保留 `LINUX_BSP` 及其路径变量。

## 目录

- `external.desc`：external 名称与描述。
- `Config.in`：Kconfig 菜单入口。
- `external.mk`：加载未来的 `package/*/*.mk` 软件包规则。

新增软件包时，应同时接入 Kconfig 与 Makefile，并保持软件包名称唯一。文档与代码的许可按各自来源记录；本项目尚未选择代码许可证。
