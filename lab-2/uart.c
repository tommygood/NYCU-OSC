/*
 * UART driver for OrangePi RV2 (SpacemiT K1) and QEMU virt.
 *
 * uart_base is set at runtime via uart_set_base() from the devicetree
 * before any UART function is called (OpenSBI already initialised the
 * hardware, so no hardcoded fallback is needed).
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

static unsigned long uart_base = 0;

/* Set base address from the devicetree (called before first UART use). */
void uart_set_base(unsigned long base) {
    uart_base = base;
}

#ifdef QEMU

char uart_getc(void) {
    volatile unsigned char *lsr = (volatile unsigned char *)(uart_base + 0x05);
    volatile unsigned char *rbr = (volatile unsigned char *)(uart_base + 0x00);
    while (!(*lsr & LSR_DR))
        ;
    return *rbr;
}

void uart_putc(char c) {
    volatile unsigned char *lsr = (volatile unsigned char *)(uart_base + 0x05);
    volatile unsigned char *thr = (volatile unsigned char *)(uart_base + 0x00);
    while (!(*lsr & LSR_THRE))
        ;
    *thr = (unsigned char)c;
}

/* Discard all bytes currently waiting in the RX FIFO. */
void uart_flush_rx(void) {
    volatile unsigned char *lsr = (volatile unsigned char *)(uart_base + 0x05);
    volatile unsigned char *rbr = (volatile unsigned char *)(uart_base + 0x00);
    while (*lsr & LSR_DR) (void)*rbr;
}

/*
 * uart_init – reset UART to 8N1, called only before jumping to a loaded
 * kernel so it starts with a clean hardware state (mirrors what OpenSBI
 * did for us).
 */
void uart_init(void) {
    /* NS16550A:
     *   IER  offset 0x01 – disable all interrupts
     *   FCR  offset 0x02 – enable FIFO, clear TX+RX FIFOs
     *   LCR  offset 0x03 – 8N1, DLAB=0
     */
    volatile unsigned char *ier = (volatile unsigned char *)(uart_base + 0x01);
    volatile unsigned char *fcr = (volatile unsigned char *)(uart_base + 0x02);
    volatile unsigned char *lcr = (volatile unsigned char *)(uart_base + 0x03);
    *ier = 0x00;
    *lcr = 0x03;
    *fcr = 0x07;
}

#else  /* OrangePi RV2 / SpacemiT K1 */

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

/* Discard all bytes currently waiting in the RX FIFO. */
void uart_flush_rx(void) {
    volatile unsigned int *lsr = (volatile unsigned int *)(uart_base + 0x14);
    volatile unsigned int *rbr = (volatile unsigned int *)(uart_base + 0x00);
    while (*lsr & LSR_DR) (void)*rbr;
}

/*
 * uart_init – reset UART to 8N1, called only before jumping to a loaded
 * kernel so it starts with a clean hardware state.
 */
void uart_init(void) {
    /* SpacemiT K1 pxa-uart:
     *   IER  offset 0x04 – bit6 UUE: UART unit enable
     *   FCR  offset 0x08 – bit5 BUS: 32-bit bus, bit0 TRFIFOE: FIFO enable
     *   LCR  offset 0x0C – bits[1:0] WLS=11: 8-bit word length
     */
    volatile unsigned int *ier = (volatile unsigned int *)(uart_base + 0x04);
    volatile unsigned int *fcr = (volatile unsigned int *)(uart_base + 0x08);
    volatile unsigned int *lcr = (volatile unsigned int *)(uart_base + 0x0C);
    *ier = (1u << 6);
    *fcr = (1u << 5) | (1u << 0);
    *lcr = 0x03;
}

#endif  /* QEMU / K1 */

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned long nibble = (h >> shift) & 0xf;
        uart_putc((char)(nibble + (nibble > 9 ? 0x57 : 0x30)));
    }
}
