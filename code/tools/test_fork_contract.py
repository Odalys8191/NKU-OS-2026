#!/usr/bin/env python3
"""Member C 2412566: real-QEMU P4, all mutations confined to .artifacts."""
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import sys
import time

root = Path(__file__).resolve().parents[1]
project = root.parents[1]
work = project / '.artifacts/lab5-p4'
logs = root.parent / 'report/logs'
logs.mkdir(parents=True, exist_ok=True)
work.mkdir(parents=True, exist_ok=True)
results = {}
for previous in logs.glob('p4-*.log'):
    if not (previous.stem.endswith(('-build', '-qemu')) or previous.stem == 'p4-grade-original'):
        continue
    for line in reversed(previous.read_text(errors='replace').splitlines()):
        if line.startswith('RESULT: '):
            results[previous.stem] = json.loads(line[8:]); break

mode = sys.argv[1] if len(sys.argv) > 1 else "all"
assert mode in ("all", "course", "contract", "grade")

def copy(name):
    tree = work / name
    if tree.exists():
        shutil.rmtree(tree)
    shutil.copytree(root, tree, ignore=shutil.ignore_patterns('build', 'obj', 'bin', 'report', '__pycache__', '.git'))
    return tree

def run(cmd, label, tree, timeout=120, markers=None):
    data = b''; complete = False; timed_out = False
    with (logs / (label + '.log')).open('wb') as out:
        out.write(('COMMAND: ' + ' '.join(map(str, cmd)) + '\nCWD: ' + str(tree) + '\n').encode())
        p = subprocess.Popen(cmd, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
        sel = selectors.DefaultSelector(); sel.register(p.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + timeout
        while p.poll() is None:
            for key, _ in sel.select(.2):
                b = key.fileobj.read1(65536); data += b; out.write(b); out.flush()
            if markers and all(x.encode() in data for x in markers):
                complete = True; break
            if time.monotonic() > deadline:
                timed_out = True; break
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
        try:
            tail, _ = p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL); tail, _ = p.communicate()
        data += tail; out.write(tail); sel.close()
        check = not timed_out and (complete if markers else p.returncode == 0)
        if markers and (b'kernel panic' in data or b'user panic' in data): check = False
        meta = dict(exit_code=p.returncode, timeout=timed_out, controlled_stop=complete,
                    check='PASS' if check else 'FAIL')
        out.write(('\nRESULT: ' + json.dumps(meta) + '\n').encode())
    results[label] = meta
    print(label, meta, flush=True)
    (logs / 'p4-results.json').write_text(json.dumps(results, indent=2) + '\n')
    return check

# Record source identity, including all uncommitted implementation and tests.
with (logs / 'p4-version.log').open('w') as out:
    for cmd in (['git', '-C', str(root.parent), 'rev-parse', 'HEAD'],
                ['git', '-C', str(root.parent), 'status', '--short', '--branch'],
                ['uname', '-m'], ['cat', '/etc/os-release'],
                ['riscv64-unknown-elf-gcc', '--version'], ['qemu-system-riscv64', '--version']):
        out.write('COMMAND: ' + ' '.join(cmd) + '\n'); out.flush()
        r = subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT)
        out.write(f'EXIT_CODE: {r.returncode}\n')
    for p in sorted(root.rglob('*')):
        if p.is_file() and (str(p.relative_to(root)).startswith(('kern/', 'libs/', 'user/', 'tools/')) or p == root / 'Makefile'):
            out.write(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(root)}\n')

qemu = ['qemu-system-riscv64', '-machine', 'virt', '-nographic', '-bios', 'default']
if mode in ("all", "course"):
    tree = copy('course')
    for prog, expected in [('forktest', ['forktest pass.']), ('forktree', ["I am '1111'"]),
                           ('exit', ['exit pass.']), ('waitkill', ['kill parent ok.', 'kill child1 ok.'])]:
        if run(['make', '-j2', 'build-' + prog], 'p4-' + prog + '-build', tree):
            run(qemu + ['-kernel', 'bin/ucore.img'], 'p4-' + prog + '-qemu', tree,
                timeout=90, markers=expected + ['all user-mode processes have quit.', 'init check memory pass.'])

# Controlled entry runs before exec in user_main, using real do_fork/exit/wait.
if mode in ("all", "contract"):
    tree = copy('contract')
    p = tree / 'kern/process/proc.c'; s = p.read_text()
    anchor = '//lab5 function3 2412566 update copy_mm'
    s = s.replace(anchor, 'int p4_fail(int);\nstatic void p4_check(void);\n' + anchor)
    s = s.replace('if (oldmm == NULL)\n    {\n        return 0;', 'if (oldmm == NULL)\n    {\n        assert(proc->mm == NULL && proc->pgdir == boot_pgdir_pa);\n        cprintf("P4 KTHREAD mm_null/boot_root PASS\\n");\n        return 0;')
    s = s.replace('proc = alloc_proc();', 'proc = p4_fail(1) ? NULL : alloc_proc();')
    s = s.replace('ret = setup_kstack(proc)', 'ret = p4_fail(2) ? -E_NO_MEM : setup_kstack(proc)')
    s = s.replace('(mm = mm_create())', '(mm = p4_fail(3) ? NULL : mm_create())')
    s = s.replace('ret = setup_pgdir(mm)', 'ret = p4_fail(4) ? -E_NO_MEM : setup_pgdir(mm)')
    s = s.replace('if (nr_process >= MAX_PROCESS)\n    {\n        local_intr_restore(intr_flag);\n        ret =', 'if (p4_fail(8) || nr_process >= MAX_PROCESS)\n    {\n        local_intr_restore(intr_flag);\n        ret =')
    s = s.replace('    (void)arg;\n#if defined(TEST)', '    (void)arg;\n    p4_check();\n#if defined(TEST)')
    s += '\n#include "../../tools/check_fork_contract.c"\n'; p.write_text(s)
    p = tree / 'kern/mm/vmm.c'; s = p.read_text(); s = 'extern int p4_fail(int);\n' + s
    s = s.replace('struct vma_struct *new = vma_create(', 'struct vma_struct *new = p4_fail(5) ? NULL : vma_create('); p.write_text(s)
    p = tree / 'kern/mm/pmm.c'; s = p.read_text(); s = 'extern int p4_fail(int);\n' + s
    s = s.replace('struct Page *page = alloc_page();\n            if (page == NULL)', 'struct Page *page = p4_fail(6) ? NULL : alloc_page();\n            if (page == NULL)')
    s = s.replace('int ret = page_insert(to, page, addr, perm);', 'int ret = p4_fail(7) ? -E_NO_MEM : page_insert(to, page, addr, perm);'); # Real intermediate page-table allocation failures inside get_pte.
    s = s.replace('if (!create || (page = alloc_page()) == NULL)',
                  'if (!create || (page = p4_fail(9) ? NULL : alloc_page()) == NULL)', 1)
    pos = s.index('if (!(*pdep0 & PTE_V))')
    s = s[:pos] + s[pos:].replace('if (!create || (page = alloc_page()) == NULL)',
                  'if (!create || (page = p4_fail(10) ? NULL : alloc_page()) == NULL)', 1)
    p.write_text(s)
    with (tree / 'Makefile').open('a') as out: out.write('\nLDFLAGS += --wrap=kmalloc --wrap=kfree\n')
    if run(['make', '-j2'], 'p4-contract-build', tree):
        run(qemu + ['-kernel', 'bin/ucore.img'], 'p4-contract-qemu', tree, markers=['P4 CONTRACT PASS', 'all user-mode processes have quit.', 'init check memory pass.'])

# Preserve original grade scoring/runner; retain its temporary copy in project.
if mode in ('all', 'grade'):
    tree = copy('grade')
    # Preserve raw evidence BEFORE the unchanged Makefile removes its temp copy.
    # SH wrapper runs the original /bin/sh grade.sh and returns its exact status.
    wrapper = work / 'grade-shell.py'
    wrapper.write_text("#!/usr/bin/env python3\nimport pathlib, shutil, subprocess, sys\n"
                       "r = subprocess.run(['/bin/sh', *sys.argv[1:]])\n"
                       f"logs = pathlib.Path({str(logs)!r})\n"
                       f"saved = pathlib.Path({str(work / 'grade-final')!r})\n"
                       "saved.mkdir(parents=True, exist_ok=True)\n"
                       "for p in pathlib.Path('.').glob('.*.log'):\n"
                       "    shutil.copy2(p, logs / ('p4-grade-raw-' + p.name[1:]))\n"
                       "for name in ['bin/kernel', 'bin/ucore.img']:\n"
                       "    p = pathlib.Path(name)\n"
                       "    if p.exists(): shutil.copy2(p, saved / p.name)\n"
                       "(logs / 'p4-grade-script-exit.log').write_text(f'COMMAND: /bin/sh tools/grade.sh\\nEXIT_CODE: {r.returncode}\\n')\n"
                       "sys.exit(r.returncode)\n")
    wrapper.chmod(0o755)
    run(['make', 'grade', 'GRADE_TMP=' + str(work / 'grade-original-tmp'),
         'SH=' + str(wrapper)], 'p4-grade-original', tree, timeout=300)
print('Evidence written to lab5/report/logs; inspect FAIL/timeout explicitly.', flush=True)
selected = [r for label, r in results.items() if mode == 'all' or (mode == 'contract' and label.startswith('p4-contract-')) or (mode == 'grade' and label.startswith('p4-grade-')) or (mode == 'course' and label.startswith(('p4-forktest-', 'p4-forktree-', 'p4-exit-', 'p4-waitkill-')))]
sys.exit(0 if all(r['check'] == 'PASS' for r in selected) else 1)
