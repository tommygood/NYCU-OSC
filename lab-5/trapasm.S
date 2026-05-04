/*
 * trap.S - Trap vector entry and exit.
 *
 * Convention: sscratch holds the kernel sp when in U-mode, 0 when in S-mode.
 */

.section .text
.global trap_entry
.align 2
trap_entry:
    /*
     * Swap sp with sscratch.
     * After swap: if from U-mode, sp = kernel sp, sscratch = user sp.
     *             if from S-mode, sp = 0, sscratch = kernel sp.
     */
    csrrw sp, sscratch, sp
    bnez  sp, _save_context

    /* We were in S-mode (sscratch was 0). Recover kernel sp from sscratch. */
    csrr  sp, sscratch

_save_context:
    addi  sp, sp, -280       /* allocate trap frame (35 * 8) */

    sd  ra,   0(sp)
    /* slot 8(sp) = user sp, filled later */
    sd  gp,  16(sp)
    sd  tp,  24(sp)
    sd  t0,  32(sp)
    sd  t1,  40(sp)
    sd  t2,  48(sp)
    sd  s0,  56(sp)
    sd  s1,  64(sp)
    sd  a0,  72(sp)
    sd  a1,  80(sp)
    sd  a2,  88(sp)
    sd  a3,  96(sp)
    sd  a4, 104(sp)
    sd  a5, 112(sp)
    sd  a6, 120(sp)
    sd  a7, 128(sp)
    sd  s2, 136(sp)
    sd  s3, 144(sp)
    sd  s4, 152(sp)
    sd  s5, 160(sp)
    sd  s6, 168(sp)
    sd  s7, 176(sp)
    sd  s8, 184(sp)
    sd  s9, 192(sp)
    sd  s10,200(sp)
    sd  s11,208(sp)
    sd  t3, 216(sp)
    sd  t4, 224(sp)
    sd  t5, 232(sp)
    sd  t6, 240(sp)

    /* Save user sp (now in sscratch) and CSRs */
    csrr  s0, sscratch
    csrr  s1, sepc
    csrr  s2, sstatus
    csrr  s3, scause
    csrr  s4, stval
    sd    s0,   8(sp)       /* user sp */
    sd    s1, 248(sp)       /* sepc    */
    sd    s2, 256(sp)       /* sstatus */
    sd    s3, 264(sp)       /* scause  */
    sd    s4, 272(sp)       /* stval   */

    /* Mark that we are now in S-mode: sscratch = 0 */
    csrw  sscratch, zero

    /* Call C handler: do_trap(struct trap_frame *tf) */
    mv    a0, sp
    call  do_trap

    /* --- Return from trap --- */
.global trap_return
trap_return:
    /* Check sstatus.SPP: if returning to U-mode, restore sscratch */
    ld    s0, 256(sp)       /* sstatus */
    andi  s0, s0, (1 << 8)  /* SPP bit */
    bnez  s0, 1f

    /* Returning to U-mode: set sscratch = kernel sp (top of frame) */
    addi  s0, sp, 280
    csrw  sscratch, s0
1:
    /* Restore sepc and sstatus */
    ld    s1, 248(sp)
    ld    s2, 256(sp)
    csrw  sepc, s1
    csrw  sstatus, s2

    /* Restore general-purpose registers */
    ld  ra,   0(sp)
    /* sp restored last */
    ld  gp,  16(sp)
    ld  tp,  24(sp)
    ld  t0,  32(sp)
    ld  t1,  40(sp)
    ld  t2,  48(sp)
    ld  s0,  56(sp)
    ld  s1,  64(sp)
    ld  a0,  72(sp)
    ld  a1,  80(sp)
    ld  a2,  88(sp)
    ld  a3,  96(sp)
    ld  a4, 104(sp)
    ld  a5, 112(sp)
    ld  a6, 120(sp)
    ld  a7, 128(sp)
    ld  s2, 136(sp)
    ld  s3, 144(sp)
    ld  s4, 152(sp)
    ld  s5, 160(sp)
    ld  s6, 168(sp)
    ld  s7, 176(sp)
    ld  s8, 184(sp)
    ld  s9, 192(sp)
    ld  s10,200(sp)
    ld  s11,208(sp)
    ld  t3, 216(sp)
    ld  t4, 224(sp)
    ld  t5, 232(sp)
    ld  t6, 240(sp)

    /* Restore sp (user sp if returning to U-mode, kernel sp if S-mode) */
    ld  sp,   8(sp)

    sret
