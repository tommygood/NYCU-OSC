/* Boot entry with self-relocation and virtual memory setup.
 *
 * OpenSBI jumps here with:
 *   a0 = hart_id
 *   a1 = DTB pointer
 */

#define PAGE_OFFSET 0xffffffc000000000

#ifdef QEMU
# define LOAD_ADDR  0x80200000
# define RELOC_ADDR 0x84000000
#else  /* OrangePi RV2 */
# define LOAD_ADDR  0x00200000
# define RELOC_ADDR 0x04000000
#endif

.section ".text.boot"
.global _start

_start:
    auipc  t0, 0                          /* t0 = runtime address of _start */
    mv     s0, a0                         /* s0 = hart_id  */
    mv     s1, a1                         /* s1 = dtb_ptr (physical) */

    /* Compare runtime address against RELOC_ADDR. */
    li     t1, RELOC_ADDR
    slli   t1, t1, 32
    srli   t1, t1, 32
    beq    t0, t1, .Lsetup_vm            /* already relocated -> skip copy */

    /* Copy image to RELOC_ADDR */
    la     t0, _start
    la     t2, _end
    li     t3, RELOC_ADDR
.Lcopy:
    ld     t4, 0(t0)
    sd     t4, 0(t3)
    addi   t0, t0, 8
    addi   t3, t3, 8
    blt    t0, t2, .Lcopy

    /* Flush instruction cache (fence.i) */
    .4byte 0x0000100F
    li     t0, RELOC_ADDR
    jr     t0                             /* re-enter _start at RELOC_ADDR */

.Lsetup_vm:
    /* Set up stack for setup_vm (C function needs stack) */
    la     sp, _end

    /* Set up page tables and enable MMU (a0 = DTB pointer, physical) */
    mv     a0, s1
    call   setup_vm

    /* MMU is now on. Identity mapping allows us to keep running.
     * Jump to the high virtual address. */
    la     t0, .Lhigh
    li     t1, PAGE_OFFSET
    add    t0, t0, t1
    jr     t0

.Lhigh:
    /* Now running at high virtual addresses.
     * All PC-relative addressing gives virtual addresses. */

    /* Update stack pointer to virtual address */
    la     sp, _end

    /* Save hart_id to global */
    la     t0, saved_hart_id
    sd     s0, 0(t0)

    /* Clear BSS (page tables are in .data, not BSS, so safe) */
    la     a3, __bss_start
    la     a4, __bss_stop
    ble    a4, a3, .Lbss_done
.Lbss:
    sd     zero, 0(a3)
    addi   a3, a3, 8
    blt    a3, a4, .Lbss
.Lbss_done:

    /* Drop identity mapping — no longer needed */
    call   drop_identity_map

    /* Set up trap vector (virtual address) and clear sscratch */
    la     t0, trap_entry
    csrw   stvec, t0
    csrw   sscratch, zero

    /* Enable SUM so kernel can access user pages (PTE_U) during syscalls */
    li     t0, (1 << 18)
    csrs   sstatus, t0

    /* Enter kernel main with DTB pointer converted to virtual address */
    li     t0, PAGE_OFFSET
    add    a0, s1, t0              /* a0 = dtb_ptr + PAGE_OFFSET (virtual) */
    tail   kernel_main


/* void jump_to_entry(unsigned long entry, unsigned long hart_id,
 *                    unsigned long dtb_ptr)
 *
 * Disables MMU and jumps to entry (physical address).
 * Must transition from virtual to physical addressing safely.
 */
.global jump_to_entry
jump_to_entry:
    mv   s0, a0          /* s0 = entry (physical) */
    mv   s1, a1          /* s1 = hart_id */
    mv   s2, a2          /* s2 = dtb_ptr (physical) */

    /* Re-enable identity mapping so we can run at physical addresses.
     * Copy kernel PGD entries to the identity (low) slots.
     * For QEMU: copy pgd[258..261] to pgd[2..5]
     * For board: copy pgd[256..257] to pgd[0..1] (already there from boot,
     *            but was cleared by drop_identity_map) */
    la   t0, _restore_identity_map
    jalr ra, t0, 0

    /* Compute physical address of the trampoline below */
    la   t0, .Lphys_trampoline
    li   t1, PAGE_OFFSET
    sub  t0, t0, t1          /* t0 = PA of trampoline */

    /* Jump to the trampoline at its physical address
     * (works because identity mapping is now active) */
    jr   t0

.Lphys_trampoline:
    /* Now running at physical address (via identity mapping) */
    /* Disable MMU */
    csrw satp, zero
    sfence.vma

    /* Set up arguments and jump to new kernel */
    mv   a0, s1              /* hart_id */
    mv   a1, s2              /* dtb_ptr */
    .4byte 0x0000100F        /* fence.i */
    jr   s0                  /* jump to entry */


/* ── switch_to(prev, next) ─────────────────────────────────────────────── */
.global switch_to
switch_to:
    /* Save prev's callee-saved registers (prev in a0) */
    sd ra,   0(a0)
    sd sp,   8(a0)
    sd s0,  16(a0)
    sd s1,  24(a0)
    sd s2,  32(a0)
    sd s3,  40(a0)
    sd s4,  48(a0)
    sd s5,  56(a0)
    sd s6,  64(a0)
    sd s7,  72(a0)
    sd s8,  80(a0)
    sd s9,  88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    /* Restore next's callee-saved registers (next in a1) */
    ld ra,   0(a1)
    ld sp,   8(a1)
    ld s0,  16(a1)
    ld s1,  24(a1)
    ld s2,  32(a1)
    ld s3,  40(a1)
    ld s4,  48(a1)
    ld s5,  56(a1)
    ld s6,  64(a1)
    ld s7,  72(a1)
    ld s8,  80(a1)
    ld s9,  88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)

    /* Set current task pointer */
    mv   tp, a1
    ret

/* ── enter_user_mode(struct trap_frame *tf) ─────────────────────────────
 * Set sp to the trap frame pointer and jump to trap_return.
 */
.global enter_user_mode
enter_user_mode:
    mv   sp, a0
    j    trap_return
