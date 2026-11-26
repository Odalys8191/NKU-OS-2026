#!/usr/bin/env python3
"""Verify SLOB reclamation and fork accounting with real QEMU in test copies."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
work = root.parents[1] / '.artifacts/lab5-p4'
logs = root.parent / 'report/logs'
logs.mkdir(parents=True, exist_ok=True)


def copy(name):
    tree = work / name
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(root, tree, ignore=shutil.ignore_patterns(
        'build', 'bin', 'obj', 'report', '__pycache__', '.git'))
    return tree


def replace(path, anchor, replacement):
    source = path.read_text()
    assert source.count(anchor) == 1, (path, anchor)
    path.write_text(source.replace(anchor, replacement))


def run(tree, name, markers, target=None):
    qemu = ['qemu-system-riscv64', '-machine', 'virt', '-nographic',
            '-bios', 'default', '-kernel', 'bin/ucore.img']
    build = ['make', '-j2'] + ([target] if target else [])
    for kind, command in [('build', build), ('qemu', qemu)]:
        log = logs / f'p4-{name}-{kind}.log'
        with log.open('w') as out:
            print(f'COMMAND: {" ".join(command)}\nCWD: {tree}', file=out, flush=True)
            try:
                code = subprocess.run(command, cwd=tree, stdin=subprocess.DEVNULL,
                                      stdout=out, stderr=subprocess.STDOUT, timeout=120).returncode
            except subprocess.TimeoutExpired:
                code = 124
            print(f'\nEXIT_CODE: {code}', file=out)
        output = log.read_text()
        okay = code == 0 and (kind == 'build' or (
            all(marker in output for marker in markers) and 'kernel panic' not in output
            and 'user panic' not in output))
        print(name, kind, 'PASS' if okay else 'FAIL', 'exit', code, flush=True)
        if not okay:
            return False
    return True


allocator = copy('slob-reclaim')
with (allocator / 'kern/mm/kmalloc.c').open('a') as out:
    out.write('\n#include "../../tools/check_slob_reclaim.c"\n')
replace(allocator / 'kern/init/init.c', '    pmm_init(); // init physical memory management',
        '    pmm_init(); // init physical memory management\n'
        '    extern void check_slob_reclaim(void);\n'
        '    check_slob_reclaim();\n'
        '    sbi_shutdown();')
replace(allocator / 'kern/init/init.c', '#include <pmm.h>', '#include <pmm.h>\n#include <sbi.h>')

accounting = copy('accounting')
proc = accounting / 'kern/process/proc.c'
anchor = 'static int\ninit_main(void *arg)\n{'
replace(proc, anchor, '#include <sbi.h>\n#include "../../tools/check_fork_accounting.c"\n' + anchor)
replace(proc, '    size_t free_pages_before = nr_free_pages();',
        '    p4_account(0);\n    size_t free_pages_before = nr_free_pages();')
replace(proc, '    assert(nr_free_pages() == free_pages_before);',
        '    p4_account(1);\n    assert(nr_free_pages() == free_pages_before);')
anchor = '    cprintf("init check memory pass.\\n");'
replace(proc, anchor, anchor + '\n    sbi_shutdown();')
with (accounting / 'kern/mm/kmalloc.c').open('a') as out:
    out.write("""
size_t p4_slob_free_bytes(void) {
    size_t total = 0;
    for (slob_t *cur = arena.next; cur != &arena; cur = cur->next) {
        total += cur->units * SLOB_UNIT;
    }
    return total;
}
""")
with (accounting / 'Makefile').open('a') as out:
    out.write('\nLDFLAGS += --wrap=kmalloc --wrap=kfree\n')

with (logs / 'p4-accounting-version.log').open('w') as out:
    subprocess.run(['git', '-C', str(root.parent), 'rev-parse', 'HEAD'], stdout=out, check=True)
    for base in (root, allocator, accounting):
        for rel in ('kern/init/init.c', 'kern/process/proc.c', 'kern/mm/kmalloc.c',
                    'tools/check_fork_accounting.c', 'tools/check_slob_reclaim.c',
                    'tools/test_fork_accounting.py', 'Makefile'):
            path = base / rel
            out.write(f'{base.name}/{rel}: {hashlib.sha256(path.read_bytes()).hexdigest()}\n')

passed = run(allocator, 'slob-reclaim', ['SLOB RECLAIM PASS'])
for prog in ('forktest', 'forktree'):
    markers = ['P4 ACCOUNTING RESTORED', 'all user-mode processes have quit.',
               'init check memory pass.']
    passed = run(accounting, f'accounting-{prog}', markers, 'build-' + prog) and passed
sys.exit(0 if passed else 1)
