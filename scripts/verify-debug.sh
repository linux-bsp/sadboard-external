#!/bin/bash
set -euo pipefail
export LC_ALL=C
verify_elf()
{
    local header
    header=$(od -v -An -tx1 -N64 "$1" | tr -d ' \n')
    [[ ${#header} -eq 128 && ${header:0:12} == 7f454c460201 && ${header:36:4} == b700 ]] || {
        echo "Expected little-endian AArch64 ELF64: $1" >&2; return 1;
    }
}
if [[ $1 == --elf ]]; then verify_elf "$2"; exit; fi
images=$(realpath "$1")
root="$images/debug"
jq="$images/../host/bin/jq"
[[ -x $jq ]] || jq=$(command -v jq)
"$jq" -e --slurpfile identity "$images/firmware-identity.json" '.firmware==$identity[0]' "$root/build-info.json" >/dev/null
while IFS=$'\t' read -r name expected; do
    path=$(realpath "$root/$name")
    [[ $path == "$root/"* && -f $path ]]
    actual=$(sha256sum "$path" | cut -d ' ' -f 1)
    [[ $actual == "$expected" ]] || { echo "Debugging file checksum mismatch: $name" >&2; exit 1; }
done < <("$jq" -r '.sha256|to_entries[]|[.key,.value]|@tsv' "$root/build-info.json")
labs=()
for name in crash cpu memory lock fd deadline backlog tcp; do
    labs+=("labs/$name-bug" "labs/$name-fixed")
done
labs+=(labs/check-result labs/case-suite labs/student-check labs/block-study)
required=(vmlinux System.map kernel.config buildroot.config busybox.config)
for name in crash.c cpu.c memory.c lock.c fd.c deadline.c backlog.c tcp.c lab.h suite.c suite.h \
    basic-cases.def suite-fault.c suite-fd.c suite-memory.c suite-cpu.c suite-sync.c suite-network.c \
    advanced-cases.def suite-advanced.h suite-performance.c suite-advanced-memory.c suite-advanced-network.c \
    suite-cgroup.c suite-peer.c suite-peer.h student-check.c block-study.c; do required+=("source/src/$name"); done
for name in "${required[@]}" "${labs[@]}"; do
    "$jq" -e --arg name "$name" '.sha256|has($name)' "$root/build-info.json" >/dev/null
done
for name in vmlinux kernel/sadboard-faults.ko kernel/sadboard-driver.ko kernel/sadboard-async.ko kernel/sadboard-sync.ko kernel/sadboard-irq.ko kernel/sadboard-power.ko labs/watchdog-probe "${labs[@]}"; do
    verify_elf "$root/$name"
    sections=$(readelf --sections --wide "$root/$name")
    [[ $sections == *'.debug_info'* ]] || { echo "Debug symbols stripped: $name" >&2; exit 1; }
done
for symbol in PROVE_LOCKING PROVE_RCU DEBUG_ATOMIC_SLEEP CMA_DEBUGFS SOFTLOCKUP_DETECTOR MAGIC_SYSRQ_SERIAL DEBUG_INFO_DWARF4 FUNCTION_TRACER \
    MEMCG COREDUMP CORE_DUMP_DEFAULT_ELF_HEADERS NET_NS VETH NET_SCH_NETEM NET_SCH_HTB NET_SCH_TBF \
    NET_SCH_INGRESS NET_CLS_U32 NET_ACT_GACT; do
    grep -Fxq "CONFIG_$symbol=y" "$root/kernel.config" || { echo "Missing kernel capability: $symbol" >&2; exit 1; }
done
grep -Fxq CONFIG_FLOAT_DURATION=y "$root/busybox.config"
"$jq" -r '"Verified debugging files: " + .tag + " / " + .firmware_id' "$images/firmware-identity.json"
