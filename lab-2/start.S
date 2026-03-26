/* Boot entry – placed first in the binary via .text.boot section.
 *
 * OpenSBI jumps here with:
 *   a0 = hart_id
 *   a1 = DTB pointer
 *
 * Boot sequence:
 *   1. Self-relocation: copy the entire bootloader image to RELOC_ADDR
 *      and jump there, freeing the standard kernel entry point (LOAD_ADDR)
 *      so that cmd_load can place a new kernel there directly.
 *   2. BSS clear at the relocated location.
 *   3. Store hart_id, set stack, call kernel_main.
 *
 * Why li, not la, for the address comparison:
 *   With -mcmodel=medany every `la` compiles to auipc+addi (PC-relative).
 *   When the copy runs at RELOC_ADDR, `la reg, _start` gives RELOC_ADDR,
 *   not the linked VMA (LOAD_ADDR).  `li reg, CONSTANT` always loads the
 *   literal value, so we use it for the comparison against LOAD_ADDR.
 *
 * Why `la` is still correct for BSS/stack after relocation:
 *   `la reg, __bss_start` from the copy at RELOC_ADDR evaluates to
 *   RELOC_ADDR + (__bss_start_vma - LOAD_ADDR), which is the correct
 *   runtime address of BSS inside the relocated copy.
 */

#ifdef QEMU
# define LOAD_ADDR  0x80200000   /* linked VMA / original entry point  */
# define RELOC_ADDR 0x84000000   /* safe destination (64 MB above base,
                                    well below QEMU DTB at ~0x88000000) */
#else  /* OrangePi RV2 */
# define LOAD_ADDR  0x00200000
# define RELOC_ADDR 0x04000000
#endif

.section ".text.boot"
.global _start

_start:
    /* auipc MUST be the first instruction so t0 = runtime address of _start
     * with zero offset adjustment needed. */
    auipc  t0, 0                          /* t0 = runtime address of _start         */
    mv     s0, a0                         /* s0 = hart_id  (preserved across copy)  */
    mv     s1, a1                         /* s1 = dtb_ptr  (preserved across copy)  */

    /* ── Relocation check ───────────────────────────────────────────────
     * Compare the runtime address of _start against RELOC_ADDR.
     * If equal, we are already running from the relocated copy → skip copy.
     *
     * Why slli+srli: `li t1, RELOC_ADDR` sign-extends on RV64 when bit 31
     * is set (e.g. 0x84000000 → 0xFFFFFFFF84000000), but auipc gives the
     * physical address with zero-extended upper bits (0x0000000084000000).
     * The shift pair clears the sign-extended upper 32 bits.
     */
    li     t1, RELOC_ADDR
    slli   t1, t1, 32                     /* clear upper 32 bits (from sign-ext)    */
    srli   t1, t1, 32                     /* t1 = 0x0000000084000000                */
    beq    t0, t1, .Ldo_bss              /* at RELOC_ADDR → already relocated      */

    /* ── Copy image to RELOC_ADDR ───────────────────────────────────────
     * Copy from _start.._end (code + rodata + data + bss + stack) to
     * RELOC_ADDR.  Use 8-byte ld/sd; _end is 16-byte aligned by the
     * linker script, so the count is always a multiple of 8.
     * Only t-registers are used; s0/s1 (hart_id/dtb_ptr) survive intact.
     */
    la     t0, _start                     /* src start                              */
    la     t2, _end                       /* src end                                */
    li     t3, RELOC_ADDR                 /* dst start                              */
.Lcopy:
    ld     t4, 0(t0)
    sd     t4, 0(t3)
    addi   t0, t0, 8
    addi   t3, t3, 8
    blt    t0, t2, .Lcopy

    /* Flush instruction cache before jumping into freshly written code.
     * fence.i = 0x0000100F (Zifencei); encoded directly for toolchain portability. */
    .4byte 0x0000100F
    li     t0, RELOC_ADDR
    jr     t0                             /* jump to _start copy; re-enters check,  */
                                          /* takes bne above → falls to .Ldo_bss    */

    /* ── BSS clear (runs at RELOC_ADDR) ─────────────────────────────────
     * la is PC-relative, so __bss_start/__bss_stop automatically resolve
     * to the correct addresses within the relocated copy.
     */
.Ldo_bss:
    la   a3, __bss_start
    la   a4, __bss_stop
    ble  a4, a3, 2f
1:  sd   zero, 0(a3)
    addi a3, a3, 8
    blt  a3, a4, 1b
2:
    /* store to now-zeroed BSS globals (defined in main.c) */
    la   t0, saved_hart_id
    sd   s0, 0(t0)

    /* stack pointer: _end is the top of the 16 KiB stack region */
    la   sp, _end

    /* call kernel_main(dtb_ptr) */
    mv   a0, s1
    tail kernel_main


/* void jump_to_entry(unsigned long entry, unsigned long hart_id,
 *                    unsigned long dtb_ptr)
 *
 * Restores the RISC-V boot calling convention (a0=hart_id, a1=dtb)
 * and branches to the loaded kernel's entry point.
 */
.global jump_to_entry
jump_to_entry:
    mv   t0, a0          /* entry address */
    mv   a0, a1          /* hart_id  → a0 */
    mv   a1, a2          /* dtb_ptr  → a1 */
    /* Flush I-cache before jumping to freshly written kernel code.
     * Required on real hardware; harmless on QEMU. */
    .4byte 0x0000100F    /* fence.i */
    jr   t0
