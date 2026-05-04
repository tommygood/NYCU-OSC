OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0x00200000;
    _start = .;
    .text   : { *(.text.boot) *(.text*)  }
    .rodata : { *(.rodata*)              }
    .data   : { *(.data*)               }
    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss*) *(COMMON)
        __bss_stop = .;
    }
    . = ALIGN(16);
    . = . + 0x4000;     /* 16 KiB stack */
    _end = .;
}
