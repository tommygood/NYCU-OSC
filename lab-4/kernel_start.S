/* Kernel entry point – no self-relocation.
 *
 * OpenSBI / bootloader jumps here with:
 *   a0 = hart_id
 *   a1 = DTB pointer
 */
.section ".text.boot"
.global _start

_start:
    mv   s0, a0                 /* s0 = hart_id  */
    mv   s1, a1                 /* s1 = dtb_ptr  */

    /* zero BSS */
    la   a3, __bss_start
    la   a4, __bss_stop
    ble  a4, a3, 2f
1:  sd   zero, 0(a3)
    addi a3, a3, 8
    blt  a3, a4, 1b
2:
    la   t0, saved_hart_id
    sd   s0, 0(t0)

    /* Set up trap vector and clear sscratch (0 = currently in S-mode) */
    la   t0, trap_entry
    csrw stvec, t0
    csrw sscratch, zero

    la   sp, _end
    mv   a0, s1
    tail kernel_main


/* void jump_to_entry(unsigned long entry, unsigned long hart_id,
 *                    unsigned long dtb_ptr)
 */
.global jump_to_entry
jump_to_entry:
    mv   t0, a0          /* entry address */
    mv   a0, a1          /* hart_id  → a0 */
    mv   a1, a2          /* dtb_ptr  → a1 */
    jr   t0


/*
 * exec_save_and_switch(unsigned long entry, unsigned long user_sp)
 *
 * Save kernel callee-saved state (setjmp-like), then switch to U-mode.
 * Never returns directly. When exec_return_to_kernel() is called
 * (from the exit syscall handler), execution resumes as if this
 * function returned.
 */
.section .data
.global exec_context
exec_context:
    .space 120   /* ra, sp, s0-s11 = 14 * 8 = 112 bytes, rounded up */

.section .text
.global exec_save_and_switch
exec_save_and_switch:
    /* Save callee-saved registers (setjmp) */
    la   t0, exec_context
    sd   ra,   0(t0)
    sd   sp,   8(t0)
    sd   s0,  16(t0)
    sd   s1,  24(t0)
    sd   s2,  32(t0)
    sd   s3,  40(t0)
    sd   s4,  48(t0)
    sd   s5,  56(t0)
    sd   s6,  64(t0)
    sd   s7,  72(t0)
    sd   s8,  80(t0)
    sd   s9,  88(t0)
    sd   s10, 96(t0)
    sd   s11, 104(t0)

    /* Now switch to U-mode (like switch_to_user) */
    /* a0 = user entry, a1 = user stack pointer */
    csrw sepc, a0

    /* Clear SPP (bit 8), set SPIE (bit 5) in sstatus */
    csrr t0, sstatus
    li   t1, (1 << 8)
    not  t1, t1
    and  t0, t0, t1       /* clear SPP */
    ori  t0, t0, (1 << 5) /* set SPIE */
    csrw sstatus, t0

    /* Store kernel sp in sscratch for trap handler */
    csrw sscratch, sp

    /* Switch to user stack and enter U-mode */
    mv   sp, a1
    sret


/*
 * exec_return_to_kernel()
 *
 * Called from the exit syscall handler in trap.c.
 * Restores kernel state saved by exec_save_and_switch (longjmp-like)
 * and returns to the caller of exec_save_and_switch.
 */
.global exec_return_to_kernel
exec_return_to_kernel:
    /* Re-enable interrupts (SIE was cleared when trap was taken) */
    csrsi sstatus, 2

    /* sscratch should be 0 in S-mode */
    csrw  sscratch, zero

    /* Restore callee-saved registers (longjmp) */
    la   t0, exec_context
    ld   ra,   0(t0)
    ld   sp,   8(t0)
    ld   s0,  16(t0)
    ld   s1,  24(t0)
    ld   s2,  32(t0)
    ld   s3,  40(t0)
    ld   s4,  48(t0)
    ld   s5,  56(t0)
    ld   s6,  64(t0)
    ld   s7,  72(t0)
    ld   s8,  80(t0)
    ld   s9,  88(t0)
    ld   s10, 96(t0)
    ld   s11, 104(t0)

    ret   /* returns to caller of exec_save_and_switch */
