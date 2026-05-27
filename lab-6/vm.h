/*
 * vm.h - Virtual memory definitions and API for Sv39.
 */
#ifndef VM_H
#define VM_H

/* Page offset: kernel virtual = physical + PAGE_OFFSET */
#define PAGE_OFFSET   0xffffffc000000000UL
#define PAGE_SIZE     (1UL << 12)     /* 4 KiB */
#define PMD_SIZE      (1UL << 21)     /* 2 MiB */
#define PGD_SIZE      (1UL << 30)     /* 1 GiB */

/* Sv39 VPN shifts */
#define PGD_SHIFT     30
#define PMD_SHIFT     21
#define PTE_SHIFT     12

#define ENTRIES_PER_TABLE  512

/* PTE descriptor bits */
#define PTE_V  (1UL << 0)   /* Valid       */
#define PTE_R  (1UL << 1)   /* Readable    */
#define PTE_W  (1UL << 2)   /* Writable    */
#define PTE_X  (1UL << 3)   /* Executable  */
#define PTE_U  (1UL << 4)   /* User        */
#define PTE_G  (1UL << 5)   /* Global      */
#define PTE_A  (1UL << 6)   /* Accessed    */
#define PTE_D  (1UL << 7)   /* Dirty       */

/* Protection combos */
#define PROT_KERNEL    (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_MMIO      (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)
#define PROT_USER_RX   (PTE_V | PTE_R | PTE_X | PTE_U | PTE_A | PTE_D)
#define PROT_USER_RW   (PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D)
#define PROT_USER_RWX  (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D)

/* SATP helpers */
#define SATP_SV39           (8UL << 60)
#define MAKE_SATP(pgd_pa)   (SATP_SV39 | ((unsigned long)(pgd_pa) >> 12))

/* PTE construction: pa -> PPN, shift into bits [53:10], OR with flags */
#define MAKE_PTE(pa, flags) ((((unsigned long)(pa)) >> 12) << 10 | (flags))

/* VA <-> PA conversion for kernel linear mapping */
#define VA_TO_PA(va)  ((unsigned long)(va) - PAGE_OFFSET)
#define PA_TO_VA(pa)  ((unsigned long)(pa) + PAGE_OFFSET)

/* PGD index where kernel virtual space starts */
#define KERNEL_PGD_INDEX  ((PAGE_OFFSET >> PGD_SHIFT) & 0x1FF)

/* How many GiB of physical memory to map */
#ifdef QEMU
# define PHYS_BASE       0x80000000UL
# define LINEAR_MAP_GIB  4
#else
# define PHYS_BASE       0x00000000UL
# define LINEAR_MAP_GIB  2  /* GiB 0-1: RAM (2GB from memory@0) */
#endif

/* VM API */
void setup_vm(const void *fdt);
void drop_identity_map(void);
void _restore_identity_map(void);

/* Get kernel PGD (for switching back to kernel address space) */
unsigned long *get_kernel_pgd(void);

/* Per-process page table API */
unsigned long *create_user_pgd(void);
void free_user_pgd(unsigned long *pgd);
void map_pages(unsigned long *pgd, unsigned long va, unsigned long pa,
               unsigned long size, unsigned long prot);
void switch_mm(unsigned long *pgd);

#endif /* VM_H */
