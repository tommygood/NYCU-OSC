/* Boot entry with self-relocation for board builds.
 *
 * OpenSBI jumps here with:
 *   a0 = hart_id
 *   a1 = DTB pointer
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

    /* Compare runtime address against RELOC_ADDR. */
    li     t1, RELOC_ADDR
    slli   t1, t1, 32
    srli   t1, t1, 32
    beq    t0, t1, .Ldo_bss              /* already relocated -> skip copy */

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
    mv   a0, a1          /* hart_id  -> a0 */
    mv   a1, a2          /* dtb_ptr  -> a1 */
    .4byte 0x0000100F    /* fence.i */
    jr   t0


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
