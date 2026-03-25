/* Boot entry – placed first in the binary via .text.boot section.
 *
 * OpenSBI jumps here with:
 *   a0 = hart_id
 *   a1 = DTB pointer
 *
 * We save both in callee-saved registers (s0, s1) across the BSS
 * clear, then store them in global variables (now that BSS is clean)
 * and call kernel_main(dtb_ptr).
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
    /* store to now-zeroed BSS globals (defined in main.c) */
    la   t0, saved_hart_id
    sd   s0, 0(t0)

    /* set stack pointer (16 KiB above BSS as defined in linker script) */
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
    jr   t0
