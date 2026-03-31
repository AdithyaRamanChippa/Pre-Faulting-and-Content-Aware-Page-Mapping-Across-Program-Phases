# E0 253: Operating Systems — Assignment 2

## Pre-Faulting and Content-Aware Page Mapping Across Program Phases

**Kernel:** Linux v6.19.3  
**Architecture:** ARM64 (aarch64)  
**Platform:** UTM/QEMU on Apple M1 MacBook Pro (Kali Linux)  
**Branch:** `assignment2`

---

## Overview

This assignment implements a Linux kernel mechanism that improves
virtual-memory behaviour across two sequential phases of a user-space
program. The kernel learns which pages of `buff1` were accessed in
Phase 1 and uses that information to pre-map corresponding pages of
`buff2` before Phase 2 begins — so Phase 2 pages incur no faults on
first access.

The extension implements content-based page deduplication: identical
pages within and across the two buffers are merged onto a single
physical frame, with copy-on-write (COW) isolation preserving
correctness.

---

## Repository Contents

```
.
├── patch/
│   └── assignment2_final.patch   # single patch against v6.19.3
├── report/
│   └── report.pdf                # design report
├── test/
│   ├── test_phase.c              # custom test program
│   ├── test_case.c               # assignment benchmark
│   └── test_pfn.c                # PFN-level COW verification
└── README.md
```

---

## Syscall Interface

Two new system calls are introduced:

| Syscall | Number | Signature |
|---------|--------|-----------|
| `sys_map_phase` | **471** | `(void *buff1, void *buff2, size_t len, int flags)` |
| `sys_dedup_pages` | **472** | `(void *buff1, void *buff2, size_t len, int flags)` |

Registered in `arch/arm64/tools/syscalls.tbl`:
```
471  common  map_phase    sys_map_phase
472  common  dedup_pages  sys_dedup_pages
```

User-space usage:
```c
#define SYS_map_phase   471
#define SYS_dedup_pages 472

syscall(SYS_map_phase,   buff1, buff2, len, 0);
/* ... Phase 1: touch buff1 pages ... */
syscall(SYS_dedup_pages, buff1, buff2, len, 0);
/* ... Phase 2: access buff2 pages fault-free ... */
```

---

## Design

### Key Decision — Deferred Sharing

Sharing frames is deferred until `sys_dedup_pages`, **not** done
eagerly during the Phase 1 fault handler. This avoids a fundamental
race on ARM64 where `memset` completes writes using a cached writable
TLB entry before write-protection can take effect.

```
Phase 1:  buff1 faults normally → record page index in bitmap
          (buff2 not mapped yet — content of buff1 still being written)

sys_dedup_pages:
  Step A — Pre-map:   buff2[i] → buff1[i]'s frame (read-only, COW)
  Step B — Intra:     identical pages within buff1 → single frame
  Step C — Cross:     identical buff1[i] vs buff2[i] → single frame
```

### Execution Flow

```
mmap(buff1)  mmap(buff2)
     │              │
     └──────┬───────┘
            ▼
   sys_map_phase()
   registers pair, allocs bitmap
            │
            ▼
   Phase 1: access buff1 pages
   do_anonymous_page() fires per page
   → phase_map_mirror_page() records page index
   → buff1 mapped normally (writable)
            │
            ▼
   sys_dedup_pages()
   Step A: wrprotect buff1[i], install buff2[i] read-only
   Step B: intra-buffer dedup within buff1
   Step C: cross-buffer dedup buff1[i] vs buff2[i]
            │
            ▼
   Phase 2: access buff2 pages
   Pre-mapped pages → zero faults
   New pages → demand-zero fault (normal)
            │
            ▼
   Write to shared page → do_wp_page() → COW copy → isolation
```

### Hook Placement

The hook is placed in `do_anonymous_page()` in `mm/memory.c`, **after**
`pte_unmap_unlock()` releases the PTE spinlock:

```c
pte_unmap_unlock(vmf->pte, vmf->ptl);
vmf->pte = NULL;   /* prevent double-unlock */

if (folio && !folio_test_large(folio))
    phase_map_mirror_page(vma, vmf->address, folio_page(folio, 0));
```

Placing the hook after lock release avoids deadlock when walking
`buff2`'s page tables (which may share the same PMD spinlock as
`buff1`).

### COW Implementation

COW is provided by the kernel's existing `do_wp_page` machinery.
The implementation sets up the correct preconditions:

- Both PTEs marked **read-only** (`PTE_RDONLY=1`)
- `PTE_DIRTY` (bit 55) and `PTE_DBM` (bit 51) **cleared**
- `PageAnonExclusive` **cleared** via `ClearPageAnonExclusive()`
- Folio reference count = **2** (one per mapping)

When a write occurs, `do_wp_page` sees `PageAnonExclusive=0` and
`folio_ref_count=2`, skips the reuse path, and calls `wp_page_copy`
to allocate a new frame — restoring isolation automatically.

---

## ARM64-Specific Challenges

| Challenge | Problem | Fix |
|-----------|---------|-----|
| `PTE_WRITE == PTE_DBM` (bit 51) | Hardware re-enables write permission | `clear_pte_bit(new, __pgprot(PTE_DBM))` |
| `PTE_DIRTY` (bit 55) | `do_wp_page` fast-path reuses frame | `clear_pte_bit(new, __pgprot(PTE_DIRTY))` |
| `PageAnonExclusive` | `do_wp_page` reuses frame unconditionally | `ClearPageAnonExclusive(page)` before sharing |
| PTE barrier ordering | Direct `*ptep =` assignment misses DSB/ISB | `ptep_modify_prot_commit()` |
| TLB staleness under QEMU | `flush_tlb_page()` insufficient in VM | `flush_tlb_range(vma, addr, addr + PAGE_SIZE)` |
| Folio API (kernel 6.x) | `page_add_anon_rmap` removed | `folio_add_anon_rmap_pte(..., RMAP_NONE)` |
| D-cache coherency | Kernel kmap access may leave stale cache | `flush_dcache_page(page1)` before PTE install |

---

## Files Modified

| File | Change |
|------|--------|
| `kernel/phase_map.c` | **New** — both syscalls + hook implementation |
| `include/linux/phase_map.h` | **New** — shared header |
| `kernel/Makefile` | `obj-y += phase_map.o` |
| `arch/arm64/tools/syscalls.tbl` | Added entries 471 and 472 |
| `mm/memory.c` | Added `#include` + hook call in `do_anonymous_page` |

---

## Building

```bash
# Inside the kernel source tree
cd ~/linux
git checkout assignment2

# Disable huge pages (required)
scripts/config --disable CONFIG_TRANSPARENT_HUGEPAGE

# Build
make ARCH=arm64 -j$(nproc)
sudo make modules_install ARCH=arm64
sudo make install ARCH=arm64
sudo reboot
```

---

## Testing

```bash
# Compile test programs
gcc -O0 -o test_phase test/test_phase.c
gcc -O0 -o test_case  test/test_case.c
gcc -O0 -o test_pfn   test/test_pfn.c

# Run tests (sudo needed for pagemap PFN access)
sudo dmesg -C && sudo ./test_phase
sudo dmesg -C && sudo ./test_case
sudo dmesg -C && sudo ./test_pfn
```

### Expected Output — test\_phase

```
buff2[page 0]: expected=0xAA got=0xAA  PASS
buff2[page 1]: expected=0xAB got=0xAB  PASS
...
COW test: buff1=0xAA (unchanged=PASS)  buff2=0xFF
COW test reverse: buff2=0xFF (unchanged=PASS)  buff1=0x11
Result: ALL TESTS PASSED
```

### Expected Output — test\_case (assignment benchmark)

```
rc=0 errno=0 (Success)
rc=0 errno=0 (Success)
seed=...  phase1=N  phase2_new=M  phase2_faults=M  mism=0  cow=OK
RESULT: PASS
```

Note: `phase2_faults` equals `phase2_new` — confirming only genuinely
new pages (never accessed in Phase 1) incur faults in Phase 2.

### PFN Verification

```bash
sudo ./test_pfn
```

```
After dedup:   buff1[0] PFN=1681ac  buff2[0] PFN=1681ac  Same? YES
After write:   buff1[0] PFN=1681ac  buff2[0] PFN=14da1c  Same? NO
buff1[0]=0xAA (unchanged)   buff2[0]=0xFF
```

---

## Limitations

1. **Single process only** — `phase_info` is a global pointer; only
   one process can use the mechanism at a time.
2. **Single buffer pair** — only one `(buff1, buff2)` pair per
   registration.
3. **No huge page support** — disabled as required by the assignment.
4. **O(n²) intra-buffer dedup** — compares every page pair; a
   production implementation would use content hashing (like KSM).
5. **No swap support** — swapped-out pages are silently skipped.
6. **Weak locking** — `mmap_read_lock` is held during PTE
   modification; concurrent `munmap`/`mremap` could race.
7. **No NUMA awareness** — frame selection ignores NUMA topology.

---

## Kernel Log

After running tests, kernel activity is visible via:

```bash
sudo dmesg | grep phase_map
```

Sample output:
```
phase_map: registered buff1=ffff86133000 buff2=ffff860fd000 pages=8
phase_map: recorded page 0
phase_map: recorded page 1
phase_map: recorded page 2
phase_map: recorded page 4
phase_map: Step A — pre-mapping buff2
phase_map: pre-mapped buff2[0]
phase_map: pre-mapped buff2[1]
phase_map: pre-mapped buff2[2]
phase_map: pre-mapped buff2[4]
phase_map: Step B — intra-buffer dedup
phase_map: Step C — cross-buffer dedup
phase_map: accessed pages: 0 1 2 4
```
