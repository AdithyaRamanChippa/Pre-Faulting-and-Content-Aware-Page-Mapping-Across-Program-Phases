#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_map_phase   471
#define SYS_dedup_pages 472
#define PAGE_SIZE       4096

static uint64_t get_pfn(void *addr)
{
    static int fd = -1;
    uint64_t entry;
    if (fd < 0) fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) { perror("pagemap"); return 0; }
    uint64_t vpn = (uint64_t)addr / PAGE_SIZE;
    if (pread(fd, &entry, 8, vpn * 8) != 8) return 0;
    if (!(entry & (1ULL << 63))) return 0;
    return entry & 0x7fffffffffffffULL;
}
 
int main(void)
{
    size_t len = 4 * PAGE_SIZE;
    char *buff1 = mmap(NULL, len, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    char *buff2 = mmap(NULL, len, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    printf("=== Step 1: register buffers ===\n");
    syscall(SYS_map_phase, buff1, buff2, len, 0);

    printf("=== Step 2: touch buff1 page 0 ===\n");
    buff1[0] = 0xAA;   /* single byte write — triggers fault */

    printf("buff1[0] PFN = %lx\n", get_pfn(buff1));
    printf("buff2[0] PFN = %lx\n", get_pfn(buff2));
    printf("Same frame after mirror? %s\n\n",
           get_pfn(buff1) == get_pfn(buff2) ? "YES" : "NO");

    printf("=== Step 3: verify buff2 readable without fault ===\n");
    printf("buff2[0] = 0x%02X (expect 0xAA)\n\n",
           (unsigned char)buff2[0]);

    printf("=== Step 4: write to buff2 — should COW ===\n");
    uint64_t pfn_before = get_pfn(buff2);
    buff2[0] = 0xFF;
    uint64_t pfn_after_b2 = get_pfn(buff2);
    uint64_t pfn_after_b1 = get_pfn(buff1);

    printf("buff1 PFN before/after: %lx / %lx\n",
           pfn_before, pfn_after_b1);
    printf("buff2 PFN before/after: %lx / %lx\n",
           pfn_before, pfn_after_b2);
    printf("buff1[0] = 0x%02X (expect 0xAA — unchanged)\n",
           (unsigned char)buff1[0]);
    printf("buff2[0] = 0x%02X (expect 0xFF)\n",
           (unsigned char)buff2[0]);
    printf("COW fired? %s\n",
           pfn_after_b1 != pfn_after_b2 ? "YES — PASS" : "NO — FAIL");

    munmap(buff1, len);
    munmap(buff2, len);
    return 0;
}
 
