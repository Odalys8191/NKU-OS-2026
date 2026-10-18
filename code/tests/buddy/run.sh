#!/usr/bin/env bash
# Run in the existing course image; no downloads or host installation.
set -euo pipefail
cd "$(dirname "$0")/../.."
out="../.artifacts/buddy"
mkdir -p "$out"
GCCPREFIX="${GCCPREFIX:-riscv64-unknown-elf-}"
"${GCCPREFIX}gcc" --version | head -n 1
qemu-system-riscv64 --version | head -n 1
uname -m
"${GCCPREFIX}gcc" -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
    -std=gnu99 -O2 -g -static -no-pie -fno-pie -fno-pic \
    -Wall -Werror -Wno-unused -fno-builtin -fno-stack-protector \
    -nostdinc -nostdlib -Ducore_test \
    -Ilibs -Ikern/debug -Ikern/mm -Ikern/driver \
    -Wl,-T,tests/best_fit/kernel.ld \
    tests/buddy/entry.S tests/buddy/check.c \
    libs/printfmt.c libs/string.c libs/sbi.c -lgcc -o "$out/check.elf"
timeout 30s qemu-system-riscv64 -machine virt -nographic -bios default \
    -kernel "$out/check.elf" > "$out/qemu.log" 2>&1 || true
if grep -E 'FAIL|kernel panic' "$out/qemu.log"; then exit 1; fi
grep -E '^PASS|buddy_check' "$out/qemu.log"
grep -q '^PASS BUDDY_MODULE' "$out/qemu.log"
