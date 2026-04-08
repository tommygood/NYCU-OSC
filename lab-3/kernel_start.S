/* Kernel entry point – no self-relocation.
 *
 * The kernel runs at its linked VMA (0x80200000 / 0x00200000) and stays
 * there.  The dedicated UART bootloader is responsible for loading the
 * kernel binary to that address before jumping here.
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

    la   sp, _end
    mv   a0, s1
    tail kernel_main


/* void jump_to_entry(unsigned long entry, unsigned long hart_id,
 *                    unsigned long dtb_ptr)
 *
 * Used by the kernel's "bootloader" command to jump back to the resident
 * bootloader at RELOC_ADDR, restoring the RISC-V boot calling convention.
 */
.global jump_to_entry
jump_to_entry:
    mv   t0, a0          /* entry address */
    mv   a0, a1          /* hart_id  → a0 */
    mv   a1, a2          /* dtb_ptr  → a1 */
    jr   t0
