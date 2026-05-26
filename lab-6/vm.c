/*
 * vm.c - Sv39 virtual memory setup.
 *
 * Sets up identity mapping (temporary) and higher-half kernel mapping
 * using 2 MiB pages at PMD level. MMIO regions are mapped with 4KB pages
 * (non-executable) for finer granularity.
 */

#include "vm.h"

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void *k_memcpy(void *dst, const void *src, unsigned long n);
extern unsigned long dtb_get_uart_base(const void *fdt);
extern unsigned long dtb_get_plic_base(const void *fdt);
extern unsigned long dtb_get_plic_size(const void *fdt);

/* Page tables — must be page-aligned and in .data (not BSS, cleared later) */
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pgd[ENTRIES_PER_TABLE] = { 0 };

static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pmd_kernel[LINEAR_MAP_GIB][ENTRIES_PER_TABLE] = { { 0 } };

static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pmd_identity[LINEAR_MAP_GIB][ENTRIES_PER_TABLE] = { { 0 } };

/*
 * For MMIO at finer granularity (4KB pages):
 * PGD[KERNEL_PGD_INDEX] → pmd_mmio → pte_mmio_* → 4KB MMIO pages
 *
 * On QEMU: UART 0x10000000, PLIC 0x0c000000, fw_cfg 0x10100000
 *          all within first 1GB (PGD index 0 for identity, KERNEL_PGD_INDEX for kernel)
 */
/*
 * MMIO page tables — shared between QEMU and board.
 * We support up to 2 MMIO GiB regions. Each gets a PMD + PTE for UART.
 * PLIC uses 2MB leaf pages in the PMD.
 */
#define MAX_MMIO_GIBS 2
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pmd_mmio[MAX_MMIO_GIBS][ENTRIES_PER_TABLE] = { { 0 } };
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pmd_mmio_identity[MAX_MMIO_GIBS][ENTRIES_PER_TABLE] = { { 0 } };

/* PTE for UART's 2MB block (4KB pages) */
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pte_uart[ENTRIES_PER_TABLE] = { 0 };
static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    pte_uart_identity[ENTRIES_PER_TABLE] = { 0 };

/* Track which GiB indices have MMIO mappings for drop_identity_map */
static int mmio_gib_indices[MAX_MMIO_GIBS];
static int mmio_gib_count = 0;

static void map_uart_4kb(unsigned long uart_base, int mmio_idx) {
    unsigned long uart_2mb_base = uart_base & ~(PMD_SIZE - 1);
    int pmd_idx = (uart_2mb_base >> PMD_SHIFT) & 0x1FF;

    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        unsigned long pa = uart_2mb_base + (unsigned long)i * PAGE_SIZE;
        pte_uart[i] = MAKE_PTE(pa, PROT_MMIO);
        pte_uart_identity[i] = MAKE_PTE(pa, PROT_MMIO);
    }
    pmd_mmio[mmio_idx][pmd_idx] = MAKE_PTE((unsigned long)pte_uart, PTE_V);
    pmd_mmio_identity[mmio_idx][pmd_idx] = MAKE_PTE((unsigned long)pte_uart_identity, PTE_V);
}

static void map_plic_2mb(unsigned long plic_base, unsigned long plic_size, int mmio_idx) {
    int pmd_start = (plic_base >> PMD_SHIFT) & 0x1FF;
    int count = (int)((plic_size + PMD_SIZE - 1) / PMD_SIZE); // num of pmd entries needed for PLIC
    for (int i = 0; i < count; i++) {
        unsigned long pa = plic_base + (unsigned long)i * PMD_SIZE;
        pmd_mmio[mmio_idx][pmd_start + i] = MAKE_PTE(pa, PROT_MMIO);
        pmd_mmio_identity[mmio_idx][pmd_start + i] = MAKE_PTE(pa, PROT_MMIO);
    }
}

static void wire_mmio_gib(int gib_idx, int mmio_idx) {
    pgd[gib_idx] = MAKE_PTE((unsigned long)pmd_mmio_identity[mmio_idx], PTE_V);
    pgd[KERNEL_PGD_INDEX + gib_idx] = MAKE_PTE((unsigned long)pmd_mmio[mmio_idx], PTE_V);
    mmio_gib_indices[mmio_gib_count++] = gib_idx;
}

static void setup_mmio(const void *fdt) {
    unsigned long uart_base = dtb_get_uart_base(fdt);
    unsigned long plic_base = dtb_get_plic_base(fdt);
    unsigned long plic_size = dtb_get_plic_size(fdt);

    int uart_gib = (int)(uart_base >> PGD_SHIFT);
    int plic_gib = (int)(plic_base >> PGD_SHIFT);

    /* Map UART's GiB */
    int mmio_idx = 0;
    map_uart_4kb(uart_base, mmio_idx);
    /* If PLIC is in the same GiB, add it to the same PMD */
    if (plic_gib == uart_gib) {
        map_plic_2mb(plic_base, plic_size, mmio_idx);
        wire_mmio_gib(uart_gib, mmio_idx);
    } else {
        wire_mmio_gib(uart_gib, mmio_idx);
        /* PLIC in a different GiB — use second PMD */
        mmio_idx = 1;
        map_plic_2mb(plic_base, plic_size, mmio_idx);
        wire_mmio_gib(plic_gib, mmio_idx);
    }
}

void setup_vm(const void *fdt)
{
    unsigned long phys_pgd_idx = PHYS_BASE >> PGD_SHIFT;

    /* Fill PMD tables with 2MiB page entries for RAM */
    for (int gib = 0; gib < LINEAR_MAP_GIB; gib++) {
        for (int entry = 0; entry < ENTRIES_PER_TABLE; entry++) {
            unsigned long pa = PHYS_BASE + (unsigned long)gib * PGD_SIZE
                             + (unsigned long)entry * PMD_SIZE;
            pmd_identity[gib][entry] = MAKE_PTE(pa, PROT_KERNEL);
            pmd_kernel[gib][entry]   = MAKE_PTE(pa, PROT_KERNEL);
        }
    }

    /* PGD entries for RAM (non-leaf, point to PMD tables) */
    for (int gib = 0; gib < LINEAR_MAP_GIB; gib++) {
        /* Identity mapping: VA = PA */
        pgd[phys_pgd_idx + gib] =
            MAKE_PTE((unsigned long)pmd_identity[gib], PTE_V);
        /* Kernel mapping: VA = PA + PAGE_OFFSET */
        pgd[KERNEL_PGD_INDEX + phys_pgd_idx + gib] =
            MAKE_PTE((unsigned long)pmd_kernel[gib], PTE_V);
    }

    /* Set up MMIO mappings (4KB for UART, 2MB for PLIC) — from DTB */
    setup_mmio(fdt);

    /* Enable MMU */
    unsigned long satp_val = MAKE_SATP((unsigned long)pgd);
    asm volatile(
        "sfence.vma\n"
        "csrw satp, %0\n"
        "sfence.vma\n"
        :
        : "r"(satp_val)
        : "memory"
    );
}

void drop_identity_map(void)
{
    unsigned long phys_pgd_idx = PHYS_BASE >> PGD_SHIFT;
    for (int gib = 0; gib < LINEAR_MAP_GIB; gib++)
        pgd[phys_pgd_idx + gib] = 0;

    /* Also clear MMIO identity mapping */
    for (int i = 0; i < mmio_gib_count; i++)
        pgd[mmio_gib_indices[i]] = 0;

    asm volatile("sfence.vma" ::: "memory");
}
