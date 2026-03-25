/*
 * UART driver for OrangePi RV2 (SpacemiT K1) and QEMU virt.
 *
 * The UART base address defaults to the known hardcoded value and can be
 * updated at runtime via uart_set_base() once the devicetree has been
 * parsed (Basic Exercise 2).
 *
 * QEMU virt  – NS16550A @ 0x10000000, 1-byte stride, byte access
 *   THR/RBR  offset 0x00 (byte)
 *   LSR      offset 0x05 (byte): bit0=DR, bit5=THRE
 *
 * K1 board   – SpacemiT pxa-uart @ 0xD4017000, 4-byte stride, word access
 *   THR/RBR  offset 0x00 (word)
 *   LSR      offset 0x14 (word): bit0=DR, bit5=TDRQ
 */

#define LSR_DR   (1u << 0)   /* data ready  (RX) */
#define LSR_THRE (1u << 5)   /* TX holding register empty / TDRQ */

#ifdef QEMU

static unsigned long uart_base = 0x10000000UL;

char uart_getc(void) {
    volatile unsigned char *lsr = (volatile unsigned char *)(uart_base + 0x05);
    volatile unsigned char *rbr = (volatile unsigned char *)(uart_base + 0x00);
    while (!(*lsr & LSR_DR))
        ;
        //asm volatile("wfi");
    return *rbr;
}

void uart_putc(char c) {
    volatile unsigned char *lsr = (volatile unsigned char *)(uart_base + 0x05);
    volatile unsigned char *thr = (volatile unsigned char *)(uart_base + 0x00);
    while (!(*lsr & LSR_THRE))
        ;
    *thr = (unsigned char)c;
}

#else  /* OrangePi RV2 / SpacemiT K1 */

static unsigned long uart_base = 0xD4017000UL;

char uart_getc(void) {
    volatile unsigned int *lsr = (volatile unsigned int *)(uart_base + 0x14);
    volatile unsigned int *rbr = (volatile unsigned int *)(uart_base + 0x00);
    while (!(*lsr & LSR_DR))
        ;
    return (char)(*rbr & 0xFF);
}

void uart_putc(char c) {
    volatile unsigned int *lsr = (volatile unsigned int *)(uart_base + 0x14);
    volatile unsigned int *thr = (volatile unsigned int *)(uart_base + 0x00);
    while (!(*lsr & LSR_THRE))
        ;
    *thr = (unsigned char)c;
}

#endif  /* QEMU / K1 */

/*
 * uart_init – configure hardware registers for 8N1 operation.
 *
 * Must be called at the very start of kernel_main (before any output).
 * This ensures UART works even when jumped to by an external bootloader
 * that may not have fully configured the UART hardware.
 *
 * We do NOT touch the baud-rate divisor (DLL/DLH) so we don't break
 * the rate already set by U-Boot / OpenSBI.
 */
void uart_init(void) {
#ifdef QEMU
    /* NS16550A:
     *   IER  offset 0x01 – disable all interrupts
     *   FCR  offset 0x02 – enable FIFO, clear TX+RX FIFOs
     *   LCR  offset 0x03 – 8N1, DLAB=0
     */
    volatile unsigned char *ier = (volatile unsigned char *)(uart_base + 0x01);
    volatile unsigned char *fcr = (volatile unsigned char *)(uart_base + 0x02);
    volatile unsigned char *lcr = (volatile unsigned char *)(uart_base + 0x03);
    *ier = 0x00;          /* no interrupts          */
    *lcr = 0x03;          /* 8N1, DLAB=0            */
    *fcr = 0x07;          /* FIFO on, clear TX+RX   */
#else
    /* SpacemiT K1 pxa-uart:
     *   IER  offset 0x04 – bit6 UUE: UART unit enable
     *   FCR  offset 0x08 – bit5 BUS: 32-bit bus, bit0 TRFIFOE: FIFO enable
     *   LCR  offset 0x0C – bits[1:0] WLS=11: 8-bit word length
     */
    volatile unsigned int *ier = (volatile unsigned int *)(uart_base + 0x04);
    volatile unsigned int *fcr = (volatile unsigned int *)(uart_base + 0x08);
    volatile unsigned int *lcr = (volatile unsigned int *)(uart_base + 0x0C);
    *ier = (1u << 6);             /* UUE: enable UART unit  */
    *fcr = (1u << 5) | (1u << 0);/* BUS=32-bit, FIFO on    */
    *lcr = 0x03;                  /* 8-bit word length      */
#endif
}

/* Update base address from the devicetree (called in kernel_main). */
void uart_set_base(unsigned long base) {
    uart_base = base;
}

/* Discard all bytes currently waiting in the RX FIFO. */
void uart_flush_rx(void) {
#ifdef QEMU
    volatile unsigned char *lsr = (volatile unsigned char *)(uart_base + 0x05);
    volatile unsigned char *rbr = (volatile unsigned char *)(uart_base + 0x00);
    while (*lsr & LSR_DR) (void)*rbr;
#else
    volatile unsigned int *lsr = (volatile unsigned int *)(uart_base + 0x14);
    volatile unsigned int *rbr = (volatile unsigned int *)(uart_base + 0x00);
    while (*lsr & LSR_DR) (void)*rbr;
#endif
}

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_putc('[');
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned long nibble = (h >> shift) & 0xf;
        uart_putc((char)(nibble + (nibble > 9 ? 0x57 : 0x30)));
    }
    uart_putc(']');
}
