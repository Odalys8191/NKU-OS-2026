#!/usr/bin/env bash
# Run inside the existing OS project image; no downloads or host installations.
set -euo pipefail
cd "$(dirname "$0")/../.."
out="../.artifacts/best-fit"
mkdir -p "$out"
riscv64-unknown-elf-gcc --version | head -n 1
qemu-system-riscv64 --version | head -n 1
uname -m
for mode in 0 1 2 3; do
    riscv64-unknown-elf-gcc -mcmodel=medany -std=gnu99 -O2 -g \
        -Wall -Werror -Wno-unused -fno-builtin -fno-stack-protector \
        -nostdinc -nostdlib -Ducore_test -DTEST_ZERO="$mode" \
        -Ilibs -Ikern/debug -Ikern/mm -Ikern/driver \
        -Wl,-T,tests/best_fit/kernel.ld \
        tests/best_fit/entry.S tests/best_fit/check.c \
        libs/printfmt.c libs/string.c libs/sbi.c -lgcc -o "$out/check-$mode.elf"
    timeout 30s qemu-system-riscv64 -machine virt -nographic -bios default \
        -kernel "$out/check-$mode.elf" > "$out/qemu-$mode.log" 2>&1
    if [[ "$mode" == 0 ]]; then
        grep -F 'PASS BEST_FIT_MODULE' "$out/qemu-$mode.log"
    else
        grep -E 'EXPECTED ASSERT .*assertion failed: n > 0' "$out/qemu-$mode.log"
    fi
    if grep -E 'FAIL|kernel panic' "$out/qemu-$mode.log"; then exit 1; fi
done
