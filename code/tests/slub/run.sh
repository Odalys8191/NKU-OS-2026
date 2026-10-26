#!/usr/bin/env bash
# Run in the existing project image. No package installation or downloads.
set -euo pipefail
cd "$(dirname "$0")/../.."
backend="${1:-best_fit}"
case "$backend" in
    best_fit) flags=();;
    buddy) flags=(-DSLUB_TEST_BUDDY);;
    *) printf 'Usage: bash code/tests/slub/run.sh {best_fit|buddy}\n' >&2; exit 2;;
esac
if [[ ! -f "kern/mm/${backend}_pmm.c" || ! -f "kern/mm/${backend}_pmm.h" ]]; then
    printf 'NOT VERIFIED: real %s PMM source/header not available\n' "$backend" >&2
    exit 2
fi
out="../.artifacts/slub-check/$backend"
mkdir -p "$out"
lab_dir="$PWD"
GCCPREFIX="${GCCPREFIX:-riscv64-unknown-elf-}"
uname -m
"${GCCPREFIX}gcc" --version | head -n 1
qemu-system-riscv64 --version | head -n 1
(
cd "$out"
"${GCCPREFIX}gcc" -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
    -std=gnu99 -O2 -g -static -no-pie -fno-pie -fno-pic \
    -Wall -Werror -Wno-unused -fno-builtin -fno-stack-protector \
    -ffunction-sections -fdata-sections -fstack-usage \
    -nostdinc -nostdlib "${flags[@]}" \
    -I"$lab_dir/libs" -I"$lab_dir/kern/debug" -I"$lab_dir/kern/mm" -I"$lab_dir/kern/driver" \
    -Wl,--gc-sections,-T,"$lab_dir/tests/best_fit/kernel.ld" \
    "$lab_dir/tests/slub/entry.S" "$lab_dir/tests/slub/check.c" "$lab_dir/kern/mm/slub.c" \
    "$lab_dir/kern/mm/${backend}_pmm.c" "$lab_dir/libs/printfmt.c" \
    "$lab_dir/libs/string.c" "$lab_dir/libs/sbi.c" -lgcc -o check.elf
) > "$out/compile.log" 2>&1 || {
        cat "$out/compile.log"; exit 1;
    }
timeout 180s qemu-system-riscv64 -machine virt -m 128M -nographic -bios default \
    -kernel "$out/check.elf" > "$out/qemu.log" 2>&1 || true
if grep -E 'FAIL|kernel panic' "$out/qemu.log"; then exit 1; fi
grep -E '^slub_check:|^PASS' "$out/qemu.log"
grep -q '^PASS SLUB_MODULE' "$out/qemu.log"
