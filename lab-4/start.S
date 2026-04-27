/* Boot entry with self-relocation for board builds.
 *
 * OpenSBI jumps here with:
 *   a0 = hart_id
 *   a1 = DTB pointer
 *
 * The kernel is linked at LOAD_ADDR (0x00200000). The bootloader command
 * receives a new kernel at the same address, so we must relocate ourselves
 * to RELOC_ADDR first to avoid self-overwrite.
 */

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
    mv     s1, a1                         /* s1 = dtb_ptr  */

    /* Compare runtime address against RELOC_ADDR.
     * slli+srli clears sign-extended upper 32 bits from li. */
    li     t1, RELOC_ADDR
    slli   t1, t1, 32
    srli   t1, t1, 32
    beq    t0, t1, .Ldo_bss              /* already relocated → skip copy */

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

.Ldo_bss:
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
    .4byte 0x0000100F    /* fence.i */
    jr   t0


/*
 * exec_save_and_switch(unsigned long entry, unsigned long user_sp)
 *
 * Save kernel callee-saved state (setjmp-like), then switch to U-mode.
 */
.section .data
.global exec_context
exec_context:
    .space 120   /* ra, sp, s0-s11 = 14 * 8 = 112 bytes, rounded up */

.section .text
.global exec_save_and_switch
exec_save_and_switch:
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

    csrw sepc, a0

    /* Clear SPP (bit 8), set SPIE (bit 5) in sstatus */
    csrr t0, sstatus
    li   t1, (1 << 8)
    not  t1, t1
    and  t0, t0, t1       /* clear SPP */
    ori  t0, t0, (1 << 5) /* set SPIE */
    csrw sstatus, t0

    csrw sscratch, sp

    mv   sp, a1
    sret


/*
 * exec_return_to_kernel()
 *
 * Called from the exit syscall handler in trap.c.
 * Restores kernel state saved by exec_save_and_switch (longjmp-like).
 */
.global exec_return_to_kernel
exec_return_to_kernel:
    csrsi sstatus, 2
    csrw  sscratch, zero

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

    ret
