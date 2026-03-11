/*
 * UART driver for OrangePi RV2 (SpacemiT K1, "ky,pxa-uart") and QEMU.
 *
 * K1 UART register map (4-byte stride, 32-bit word access):
 *   Offset  Register
 *   0x00    RBR (read) / THR (write)  [DLL when DLAB=1]
 *   0x04    IER  — bit 6: UUE (UART Unit Enable)  [DLH when DLAB=1]
 *   0x08    FCR  — bit 5: BUS (32-bit bus), bit 0: TRFIFOE
 *   0x0C    LCR  — bit 7: DLAB, bits[1:0]: WLS (11=8-bit)
 *   0x10    MCR  — bit 4: LOOP, bit 1: RTS, bit 0: DTR
 *   0x14    LSR  — bit 0: DR (data ready), bit 5: TDRQ (TX request)
 *
 * TX and RX are both done via direct MMIO on UART0 (0xD4017000).
 * The RX pinmux must be explicitly configured — U-Boot leaves it in GPIO
 * mode because it uses SBI ecalls for input and never sets the RX pin.
 *
 * QEMU uses ns16550a at 0x10000000, 1-byte stride, byte access.
 */

#define LSR_DR  (1u << 0)
#define LSR_THRE   (1u << 5)   /* TDRQ: TX holding register ready */

#ifdef QEMU

#define UART_RBR   ((volatile unsigned char *)(0x10000000UL + 0x00))
#define UART_THR   ((volatile unsigned char *)(0x10000000UL + 0x00))
#define UART_LSR   ((volatile unsigned char *)(0x10000000UL + 0x05))

char uart_getc(void) {
    while (!(*UART_LSR & LSR_DR))
        ;
    char c = *UART_RBR;
    return (c == '\r') ? '\n' : c;
}

#else  /* real board: OrangePi RV2 / SpacemiT K1 */

/* forward declarations — defined in the common section below */
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);

/* UART0 (DTB serial0, K1 address map: 0xD4017000): TX and RX */
#define UART_RBR   ((volatile unsigned int *)(0xD4017000UL + 0x00))
#define UART_THR   ((volatile unsigned int *)(0xD4017000UL + 0x00))
#define UART_LSR   ((volatile unsigned int *)(0xD4017000UL + 0x14))

char uart_getc(void) {
    while (!(*UART_LSR & LSR_DR))
        ;
    char c = (char)(*UART_RBR & 0xFF);
    return (c == '\r') ? '\n' : c;
}

#endif  /* QEMU / real board */

/* ── common TX path (direct MMIO works from S-mode on K1) ─────────────── */


void uart_putc(char c) {
    if (c == '\n') {
        while (!(*UART_LSR & LSR_THRE))
            ;
        *UART_THR = '\r';
    }
    while (!(*UART_LSR & LSR_THRE))
        ;
    *UART_THR = (unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned long nibble = (h >> shift) & 0xf;
        uart_putc((char)(nibble + (nibble > 9 ? 0x57 : '0')));
    }
}
