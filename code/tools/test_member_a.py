#!/usr/bin/env python3
"""Run A's tests in a disposable Linux copy; never patch the submitted kernel."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
work = Path(tempfile.mkdtemp(prefix="lab4-member-a-"))
shutil.copytree(root, work, dirs_exist_ok=True, ignore=shutil.ignore_patterns("obj", "bin", ".git"))
logs = root / "docs" / "validation"
logs.mkdir(parents=True, exist_ok=True)
print("Temporary test copy:", work, flush=True)

official = (work / "tools/test_lab4.c").read_text()
official = official[:official.index("void test_lab4_process_create")]
extra = r'''
#include <error.h>
#include <string.h>
static const struct pmm_manager *real_manager;
static int allocation_budget;
static struct Page *limited_alloc(size_t n) {
    if (allocation_budget == 0) return NULL;
    allocation_budget--;
    return real_manager->alloc_pages(n);
}
void test_member_a_failure(void) {
    size_t before = nr_free_pages();
    struct Page *root = alloc_page(), *data = alloc_page();
    assert(root && data);
    pde_t *pgdir = page2kva(root);
    memset(pgdir, 0, PGSIZE);
    real_manager = pmm_manager;
    struct pmm_manager limited = *real_manager;
    limited.alloc_pages = limited_alloc;
    for (int budget = 0; budget <= 1; budget++) {
        size_t free_before = nr_free_pages();
        allocation_budget = budget;
        pmm_manager = &limited;
        assert(page_insert(pgdir, data, 0, PTE_R) == -E_NO_MEM);
        pmm_manager = real_manager;
        assert(page_ref(data) == 0 && get_page(pgdir, 0, NULL) == NULL);
        page_remove(pgdir, 0);
        // Existing get_pte retains a first-level allocation on later failure.
        assert(nr_free_pages() == free_before - budget);
        if (pgdir[0] & PTE_V) {
            free_page(pde2page(pgdir[0]));
            pgdir[0] = 0;
        }
        assert(nr_free_pages() == free_before);
    }
    free_page(data);
    free_page(root);
    assert(nr_free_pages() == before);
    cprintf("[Test PMM allocation failure] PASS\n");
}
'''
(work / "kern/mm/member_a_test.c").write_text(official + extra)
p = work / "kern/init/init.c"
s = p.read_text().replace("#include <dtb.h>", "#include <dtb.h>\n#include <sbi.h>\nvoid test_lab4_pmm_basic(void);\nvoid test_member_a_failure(void);")
s = s.replace("    proc_init(); // init process table", """    test_lab4_pmm_basic();
    test_member_a_failure();
    clock_init();
    intr_enable();
    while (ticks < 305) { asm volatile("wfi"); }
    intr_disable();
    cprintf("[Test trap timer 305 ticks] PASS\\n");
    cprintf("[Member A isolated tests] PASS\\n");
    sbi_shutdown();
    while (1) {}
    // Process creation and scheduling deliberately excluded from this test.
""")
p.write_text(s)
cmd = ["make", "-j2", "GCCPREFIX=riscv64-linux-gnu-",
       "DEFS=-Dsbadaddr=stval -Wno-error=int-conversion -fno-pie -march=rv64imac_zicsr_zifencei -mabi=lp64"]
result = subprocess.run(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(logs / "member_A_build.log").write_bytes(result.stdout)
if result.returncode:
    print(result.stdout.decode(errors="replace")[-10000:])
    raise SystemExit(result.returncode)
cmd = ["qemu-system-riscv64", "-machine", "virt", "-nographic", "-bios", "default",
       "-kernel", "bin/ucore.img"]
try:
    result = subprocess.run(cmd, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    output = result.stdout
except subprocess.TimeoutExpired as error:
    output = error.stdout or b""
(logs / "member_A_qemu.log").write_bytes(output)
print(output.decode(errors="replace"))
assert b"[Member A isolated tests] PASS" in output, "Isolated tests did not finish"
assert b"kernel panic" not in output
