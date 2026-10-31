#!/usr/bin/env python3
"""Compose the BSP defconfig and SadBoard fragments in an isolated output tree."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def main():
    external = Path(__file__).resolve().parents[1]
    workspace = external.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=workspace / "output/am625-debug")
    parser.add_argument("--toolchain", type=Path, help="optional existing AArch64 GCC 15/glibc toolchain, Linux 7.0 headers")
    parser.add_argument("--dl-dir", type=Path, help="optional shared Buildroot download cache")
    parser.add_argument("--kernel-extra", action="append", choices=["kgdb", "memory"], default=[])
    args = parser.parse_args()
    output = args.output.resolve()
    if (output / ".config").exists():
        parser.error("output already contains .config; use a new output directory to preserve it")
    for path in (workspace, output):
        if any(c.isspace() for c in str(path)):
            parser.error("Buildroot paths must not contain whitespace")
    bsp = workspace / "bsp-base-external"
    buildroot = workspace / "buildroot"
    base = bsp / "configs/alientek_am62x_defconfig"
    if not base.is_file() or not (buildroot / "Makefile").is_file():
        parser.error("initialize buildroot and bsp-base-external submodules first")
    local = []
    if args.toolchain:
        sdk = args.toolchain.resolve()
        gcc = sdk / "bin/aarch64-buildroot-linux-gnu-gcc"
        if not gcc.is_file():
            parser.error("toolchain must contain bin/aarch64-buildroot-linux-gnu-gcc")
        version = subprocess.check_output([str(gcc), "-dumpversion"], text=True).strip()
        if version.split(".")[0] != "15":
            parser.error("this optional toolchain profile expects GCC 15")
        local += ["# BR2_TOOLCHAIN_BUILDROOT is not set", "BR2_TOOLCHAIN_EXTERNAL=y",
                  "BR2_TOOLCHAIN_EXTERNAL_CUSTOM=y", "BR2_TOOLCHAIN_EXTERNAL_PREINSTALLED=y",
                  f'BR2_TOOLCHAIN_EXTERNAL_PATH="{sdk}"',
                  'BR2_TOOLCHAIN_EXTERNAL_CUSTOM_PREFIX="aarch64-buildroot-linux-gnu"',
                  "BR2_TOOLCHAIN_EXTERNAL_GCC_15=y", "BR2_TOOLCHAIN_EXTERNAL_HEADERS_7_0=y",
                  "BR2_TOOLCHAIN_EXTERNAL_CUSTOM_GLIBC=y", "BR2_TOOLCHAIN_EXTERNAL_CXX=y",
                  "# BR2_TOOLCHAIN_EXTERNAL_INET_RPC is not set",
                  "# BR2_TOOLCHAIN_EXTERNAL_GDB_SERVER_COPY is not set"]
    if args.dl_dir:
        local.append(f'BR2_DL_DIR="{args.dl_dir.resolve()}"')
    if args.kernel_extra:
        kernel_files = ["debug"] + list(dict.fromkeys(args.kernel_extra))
        value = " ".join("$(BR2_EXTERNAL_SADBOARD_PATH)/kernel/" + x + ".config" for x in kernel_files)
        local.append(f'BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="{value}"')
    output.mkdir(parents=True, exist_ok=True)
    local_file = output / "local-options.config"
    local_file.write_text("\n".join(local) + "\n")
    fragments = [base, external / "configs/debug-tools.config", external / "board/am625/debug.config", local_file]
    env = dict(os.environ, CONFIG_="BR2_", KCONFIG_CONFIG=str(output / ".config"))
    subprocess.run([str(buildroot / "support/kconfig/merge_config.sh"), "-m", "-O", str(output), *map(str, fragments)], cwd=buildroot, env=env, check=True)
    command = ["make", "-C", str(buildroot), f"O={output}", f"BR2_EXTERNAL={bsp} {external}"]
    subprocess.run(command + ["olddefconfig"], check=True)
    config = (output / ".config").read_text()
    required = ["BR2_ENABLE_DEBUG", "BR2_PACKAGE_GDB_SERVER", "BR2_PACKAGE_STRACE", "BR2_PACKAGE_LTRACE",
                "BR2_PACKAGE_VALGRIND", "BR2_PACKAGE_LINUX_TOOLS_PERF", "BR2_PACKAGE_TRACE_CMD",
                "BR2_PACKAGE_IPERF3", "BR2_PACKAGE_TCPDUMP", "BR2_TARGET_ROOTFS_TAR"]
    missing = [key for key in required if not re.search(r"^" + key + r"=y$", config, re.M)]
    if missing or "BR2_STRIP_strip=y" in config:
        raise SystemExit(f"debug configuration validation failed: {missing}; check strip setting")
    print(f"Configured {output} (no firmware built).")
    print(f"Build with: make -C {output}")


if __name__ == "__main__":
    main()
