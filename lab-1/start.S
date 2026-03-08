/* Boot entry: placed first via .text.boot section */
.section ".text.boot"
.global _start
_start:
    /* Zero out BSS */
    la   a3, __bss_start
    la   a4, __bss_stop
    ble  a4, a3, clear_bss_done
clear_bss:
    sd   zero, (a3)
    add  a3, a3, 8
    blt  a3, a4, clear_bss
clear_bss_done:
    /* Set stack pointer to top of allocated stack region */
    la   sp, _end
    tail start_kernel
