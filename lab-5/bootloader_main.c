/*
 * Dedicated UART bootloader.
 *
 * Linked at LOAD_ADDR, self-relocates to RELOC_ADDR at startup (start.S).
 * After relocation:
 *   1. Parses DTB for UART base address.
 *   2. Loops waiting for the BOOT protocol over UART.
 *   3. Loads the received kernel image to LOAD_ADDR.
 *   4. Jumps to LOAD_ADDR.
 *
 * When the loaded kernel executes a "bootloader" command, it can jump back
 * to RELOC_ADDR, which re-runs this flow from _start (start.S re-inits BSS
 * and calls kernel_main again).
 *
 * BOOT protocol (little-endian):
 *   [4 bytes] magic  = 0x544F4F42  ("BOOT")
 *   [4 bytes] size   = byte count of the kernel binary
 *   [size bytes]     kernel binary
 */

typedef unsigned char uint8_t;

/* ── External symbols ────────────────────────────────────────────────────── */
extern char          uart_getc(void);
extern void          uart_puts(const char *s);
extern void          uart_hex(unsigned long h);
extern void          uart_putdec(unsigned long n);
extern void          uart_init(void);
extern void          uart_set_base(unsigned long base);
extern unsigned long uart_read_u32_le(void);
extern void          jump_to_entry(unsigned long entry, unsigned long hart_id,
                                   unsigned long dtb_ptr);
extern unsigned long dtb_get_uart_base(const void *fdt);

/* ── Globals ─────────────────────────────────────────────────────────────── */
unsigned long saved_hart_id;   /* written by start.S */

#ifdef QEMU
# define LOAD_ADDR 0x80200000UL
#else
# define LOAD_ADDR 0x00200000UL
#endif

/* ── Entry point (called by start.S as "kernel_main") ───────────────────── */
void kernel_main(void *fdt) {
    unsigned long uart_base = dtb_get_uart_base(fdt);
    if (uart_base) uart_set_base(uart_base);

    uart_puts("\r\nUART Bootloader ready. Waiting for kernel (BOOT protocol)...\r\n");

    /* Flush once to clear any echo bytes from the startup message above. */

    while (1) {
        unsigned long magic = uart_read_u32_le();
        if (magic != 0x544F4F42UL) {
            uart_puts("Bad magic, retrying...\r\n");
            continue;
        }

        unsigned long size = uart_read_u32_le();
        uart_puts("Receiving ");
        uart_putdec(size);
        uart_puts(" bytes to ");
        uart_hex(LOAD_ADDR);
        uart_puts("...\r\n");

        uint8_t *dst = (uint8_t *)LOAD_ADDR;
        for (unsigned long i = 0; i < size; i++)
            dst[i] = (uint8_t)uart_getc();

        uart_puts("Done. Jumping to ");
        uart_hex(LOAD_ADDR);
        uart_puts("\r\n");

        uart_init();
        jump_to_entry(LOAD_ADDR, saved_hart_id, (unsigned long)fdt);
        while (1) {}   /* unreachable */
    }
}
