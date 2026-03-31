#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_map_phase   471
#define SYS_dedup_pages 472
#define PAGE_SIZE       4096
#define NUM_PAGES       8

/* ── syscall wrappers ── */
static long sys_map_phase(void *b1, void *b2, size_t len, int flags)
{
    return syscall(SYS_map_phase, b1, b2, len, flags);
}

static long sys_dedup_pages(void *b1, void *b2, size_t len, int flags)
{
    return syscall(SYS_dedup_pages, b1, b2, len, flags);
}

/* ── get physical frame number via /proc/self/pagemap ── */
static uint64_t get_pfn(void *addr)
{
    static int fd = -1;
    uint64_t entry;
    if (fd < 0) fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;
    uint64_t vpn = (uint64_t)addr / PAGE_SIZE;
    if (pread(fd, &entry, 8, vpn * 8) != 8) return 0;
    if (!(entry & (1ULL << 63))) return 0;
    return entry & 0x7fffffffffffffULL;
}

/* ── check if a page is resident via mincore ── */
static int page_is_mapped(char *addr)
{
    unsigned char vec = 0;
    if (mincore(addr, PAGE_SIZE, &vec) == 0)
        return vec & 1;
    return -1;
}

/* ── print residency status of all pages in a buffer ── */
static void print_map_status(const char *label, char *buf, int npages)
{
    printf("  [%s] residency: ", label);
    for (int i = 0; i < npages; i++) {
        int r = page_is_mapped(buf + i * PAGE_SIZE);
        printf("pg%-2d:%s  ", i, r == 1 ? "YES" : (r == 0 ? "no " : "ERR"));
    }
    printf("\n");
}

int main(void)
{
    size_t len = NUM_PAGES * PAGE_SIZE;
    long ret;
    int all_ok = 1;

    printf("=================================================\n");
    printf("  OS Assignment 2 — Phase Map + Dedup Test\n");
    printf("=================================================\n\n");

    /* ── Allocate buffers ── */
    char *buff1 = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *buff2 = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (buff1 == MAP_FAILED || buff2 == MAP_FAILED) {
        perror("mmap"); return 1;
    }

    printf("buff1 = %p\n", buff1);
    printf("buff2 = %p\n\n", buff2);

    /* ── Before anything ── */
    printf("--- Before sys_map_phase ---\n");
    print_map_status("buff1", buff1, NUM_PAGES);
    print_map_status("buff2", buff2, NUM_PAGES);
    printf("\n");

    /* ── Register buffer pair ── */
    ret = sys_map_phase(buff1, buff2, len, 0);
    printf("sys_map_phase returned: %ld\n\n", ret);

    /* ── Phase 1: touch only SOME pages of buff1 ── */
    printf("--- Phase 1: touching buff1 pages 0,1,2,4 only ---\n");
    int touched[] = {0, 1, 2, 4};
    int n_touched = 4;

    for (int i = 0; i < n_touched; i++) {
        int pg = touched[i];
        memset(buff1 + pg * PAGE_SIZE, 0xAA + pg, PAGE_SIZE);
        printf("  touched buff1[page %d] with pattern 0x%02X\n",
               pg, 0xAA + pg);
    }
    printf("\n");

    /* ── Check residency after Phase 1 ── */
    printf("--- After Phase 1 ---\n");
    print_map_status("buff1", buff1, NUM_PAGES);
    print_map_status("buff2", buff2, NUM_PAGES);
    printf("\n");

    /* ── Dedup ── */
    printf("--- Calling sys_dedup_pages ---\n");
    ret = sys_dedup_pages(buff1, buff2, len, 0);
    printf("sys_dedup_pages returned: %ld\n\n", ret);

    /* ── Check residency after dedup ── */
    printf("--- After sys_dedup_pages ---\n");
    print_map_status("buff1", buff1, NUM_PAGES);
    print_map_status("buff2", buff2, NUM_PAGES);
    printf("\n");

    /* ── Verify buff2 has correct content WITHOUT Phase 2 access ── */
    printf("--- Verifying buff2 content (no explicit access) ---\n");
    for (int i = 0; i < n_touched; i++) {
        int pg = touched[i];
        unsigned char expected = 0xAA + pg;
        unsigned char actual   = (unsigned char)buff2[pg * PAGE_SIZE];
        int ok = (actual == expected);
        printf("  buff2[page %d]: expected=0x%02X got=0x%02X  %s\n",
               pg, expected, actual, ok ? "PASS" : "FAIL");
        if (!ok) all_ok = 0;
    }
    printf("\n");

    /* ── Verify unaccessed pages of buff2 are NOT mapped ── */
    printf("--- Checking unaccessed pages are NOT pre-mapped ---\n");
    int untouched[] = {3, 5, 6, 7};
    for (int i = 0; i < 4; i++) {
        int pg = untouched[i];
        int mapped = page_is_mapped(buff2 + pg * PAGE_SIZE);
        int ok = (mapped == 0);
        printf("  buff2[page %d]: %s (should be unmapped)\n",
               pg, ok ? "PASS — not mapped" : "FAIL — mapped");
        if (!ok) all_ok = 0;
    }
    printf("\n");

    /* ── Phase 2: read buff2 ── */
    printf("--- Phase 2: reading buff2 (should be fault-free) ---\n");
    for (int i = 0; i < n_touched; i++) {
        int pg = touched[i];
        unsigned char val = (unsigned char)buff2[pg * PAGE_SIZE];
        printf("  buff2[page %d] = 0x%02X\n", pg, val);
    }
    printf("\n");

    /* ── PFN check before COW ── */
    printf("--- PFN check: buff1 and buff2 share frames after dedup ---\n");
    for (int i = 0; i < n_touched; i++) {
        int pg = touched[i];
        uint64_t pfn1 = get_pfn(buff1 + pg * PAGE_SIZE);
        uint64_t pfn2 = get_pfn(buff2 + pg * PAGE_SIZE);
        int shared = (pfn1 != 0 && pfn1 == pfn2);
        printf("  page %d: buff1 PFN=%lx  buff2 PFN=%lx  shared=%s\n",
               pg, pfn1, pfn2, shared ? "YES" : "NO");
    }
    printf("\n");

    /* ── COW test: write to buff2, check buff1 is unaffected ── */
    printf("--- COW test: writing to buff2, checking buff1 isolation ---\n");
    for (int i = 0; i < n_touched; i++) {
        int pg = touched[i];
        unsigned char before_b1 = (unsigned char)buff1[pg * PAGE_SIZE];
        buff2[pg * PAGE_SIZE] = 0xFF;
        unsigned char after_b1  = (unsigned char)buff1[pg * PAGE_SIZE];
        uint64_t pfn1_after = get_pfn(buff1 + pg * PAGE_SIZE);
        uint64_t pfn2_after = get_pfn(buff2 + pg * PAGE_SIZE);
        int isolated = (after_b1 == before_b1);
        int split    = (pfn1_after != pfn2_after);
        printf("  page %d: buff1=0x%02X (unchanged=%s)  buff2=0x%02X  PFN split=%s\n",
               pg, after_b1,
               isolated ? "PASS" : "FAIL",
               (unsigned char)buff2[pg * PAGE_SIZE],
               split ? "YES" : "NO");
        if (!isolated) all_ok = 0;
    }
    printf("\n");

    /* ── COW reverse: write to buff1, check buff2 is unaffected ── */
    printf("--- COW test reverse: writing to buff1 ---\n");
    for (int i = 0; i < n_touched; i++) {
        int pg = touched[i];
        unsigned char before_b2 = (unsigned char)buff2[pg * PAGE_SIZE];
        buff1[pg * PAGE_SIZE] = 0x11;
        unsigned char after_b2  = (unsigned char)buff2[pg * PAGE_SIZE];
        int isolated = (after_b2 == before_b2);
        printf("  page %d: buff2=0x%02X (unchanged=%s)  buff1=0x%02X\n",
               pg, after_b2,
               isolated ? "PASS" : "FAIL",
               (unsigned char)buff1[pg * PAGE_SIZE]);
        if (!isolated) all_ok = 0;
    }
    printf("\n");

    /* ── Dedup within buff1: pages 0 and 1 have same content after COW ──
       Write same pattern to two pages, then check they share a frame       */
    printf("--- Intra-buffer dedup test (buff1 pages with same content) ---\n");
    {
        /* Write identical content to buff1 pages 5 and 6 */
        memset(buff1 + 5 * PAGE_SIZE, 0xBB, PAGE_SIZE);
        memset(buff1 + 6 * PAGE_SIZE, 0xBB, PAGE_SIZE);

        /* Re-register and dedup */
        char *b1b = buff1;
        char *b2b = buff2;
        sys_map_phase(b1b, b2b, len, 0);

        /* Touch pages 5 and 6 (already written above) */

        sys_dedup_pages(b1b, b2b, len, 0);

        uint64_t pfn5 = get_pfn(buff1 + 5 * PAGE_SIZE);
        uint64_t pfn6 = get_pfn(buff1 + 6 * PAGE_SIZE);
        int deduped = (pfn5 != 0 && pfn5 == pfn6);
        printf("  buff1[5] PFN=%lx  buff1[6] PFN=%lx  deduped=%s\n",
               pfn5, pfn6, deduped ? "YES" : "NO (may need explicit dedup touch)");
    }
    printf("\n");

    /* ── Final summary ── */
    printf("=================================================\n");
    printf("  Result: %s\n", all_ok ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    printf("=================================================\n\n");
    printf("Check kernel log with:  sudo dmesg | grep phase_map\n\n");

    munmap(buff1, len);
    munmap(buff2, len);
    return all_ok ? 0 : 1;
}
