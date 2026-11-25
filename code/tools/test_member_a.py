#!/usr/bin/env python3
"""Run A's production code in a temporary kernel with explicit test fixtures.

Run with Linux Python from WSL. Does not fill B/C production exercises, modify
grade.sh, or claim make grade success. Retains temporary tree for inspection.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
def source_hashes():
    paths = [root / "Makefile"]
    for folder in ("kern", "libs", "user"):
        paths.extend(p for p in sorted((root / folder).rglob("*")) if p.is_file())
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}

source_snapshot = source_hashes()
logs = root / "report" / "logs"
logs.mkdir(parents=True, exist_ok=True)
work = Path(tempfile.mkdtemp(prefix="lab5-member-a-"))
tree = work / "project"
shutil.copytree(root, tree, ignore=shutil.ignore_patterns("obj", "bin", ".git", "logs", "__pycache__"))

proc_path = tree / "kern/process/proc.c"
proc = proc_path.read_text()
match = re.search(r"static int\s+init_main\(void \*arg\)\s*\{", proc)
if match is None:
    raise RuntimeError("migrated init_main not found")
start = proc.index("{", match.start())
end, depth = start + 1, 1
while depth:
    depth += (proc[end] == "{") - (proc[end] == "}")
    end += 1
proc = proc[:start] + "{ return member_a_suite(); }" + proc[end:]
proc = '#include "../../tools/test_lab5.h"\nstatic int member_a_suite(void);\n' + proc
proc += '\n#include "../../tools/member_a_fixture.inc"\n'
# Check the teacher's actual process-creation assertions before init runs.
proc = proc.replace('assert(initproc->pid == 1 && initproc->parent == idleproc);',
                    'assert(initproc->pid == 1 && initproc->parent == idleproc);\n    test_lab5_process_create();')
proc_path.write_text(proc)
shutil.copyfile(root / "tools/member_a_probe.c", tree / "user/member_a_probe.c")

def run(command, logfile, timeout=90):
    with open(logs / logfile, "w") as out:
        out.write("COMMAND: " + " ".join(command) + "\n")
        out.flush()
        try:
            result = subprocess.run(command, cwd=tree, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 124
        out.write("\nEXIT_CODE: " + str(code) + "\n")
    print(logfile, "exit", code, flush=True)
    return code

print("Temporary test tree:", tree, flush=True)
with open(logs / "member-a-environment.log", "w") as out:
    for command in (["uname", "-a"], ["riscv64-unknown-elf-gcc", "--version"],
                    ["qemu-system-riscv64", "--version"], ["python3", "--version"],
                    ["make", "--version"]):
        out.write("COMMAND: " + " ".join(command) + "\n")
        out.flush()
        subprocess.run(command, stdout=out, stderr=subprocess.STDOUT, check=True)
build = run(["make", "-j4"], "member-a-fixture-build.log", 180)
qemu = None
if build == 0:
    qemu = run(["qemu-system-riscv64", "-machine", "virt", "-nographic", "-bios", "default",
                "-kernel", "bin/ucore.img"], "member-a-qemu.log", 45)

output = (logs / "member-a-qemu.log").read_text() if qemu is not None else ""
unchanged = source_snapshot == source_hashes()
passed = build == 0 and qemu == 0 and unchanged and "[Member A] ALL TESTS PASS" in output and "kernel panic" not in output
manifest = {
    "temporary_tree": str(tree), "build_exit": build, "qemu_exit": qemu,
    "passed": passed, "scope": "member A fixture tests, not full make grade",
    "sources_unchanged_during_test": unchanged,
    "production_sources_sha256": source_snapshot,
}
(logs / "member-a-result.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
print("PASS" if passed else "FAIL", flush=True)
if not passed:
    path = logs / ("member-a-fixture-build.log" if build else "member-a-qemu.log")
    print(path.read_text()[-5000:], flush=True)
raise SystemExit(0 if passed else 1)
