/*
 * UART driver for OrangePi RV2 (SpacemiT K1, "ky,pxa-uart") and QEMU.
 *
 * K1 UART register map (4-byte stride, 32-bit word access):
 *   Offset  Register
 *   0x00    RBR (read) / THR (write)
 *   0x04    IER  — bit 6: UUE (UART Unit Enable, must = 1)
 *   0x08    FCR  — bit 5: BUS (32-bit bus, must = 1 for valid RBR/THR data)
 *                  bit 2: RESETTF, bit 1: RESETRF, bit 0: TRFIFOE
 *   0x14    LSR  — bit 0: DR (data ready), bit 5: TDRQ (TX request)
 *
 * QEMU uses ns16550a at 0x10000000, 1-byte stride, byte access.
 */

#ifdef QEMU

#define UART_RBR   ((volatile unsigned char *)(0x10000000UL + 0x00))
#define UART_THR   ((volatile unsigned char *)(0x10000000UL + 0x00))
#define UART_FCR   ((volatile unsigned char *)(0x10000000UL + 0x02))
#define UART_LSR   ((volatile unsigned char *)(0x10000000UL + 0x05))

void uart_init(void) {
    /* Standard 16550A: enable+reset FIFOs, trigger level = 1 byte */
    *UART_FCR = 0x07;
}

#else

#define UART_RBR   ((volatile unsigned int *)(0xD4017000UL + 0x00))
#define UART_THR   ((volatile unsigned int *)(0xD4017000UL + 0x00))
#define UART_IER   ((volatile unsigned int *)(0xD4017000UL + 0x04))
#define UART_FCR   ((volatile unsigned int *)(0xD4017000UL + 0x08))
#define UART_LSR   ((volatile unsigned int *)(0xD4017000UL + 0x14))

void uart_init(void) {
    /* IER: UUE=1 (bit 6) — UART unit enable; without this TX and RX are off */
    *UART_IER = (1u << 6);
    /*
     * FCR:
     *   bit 5 (BUS)     = 1 — 32-bit peripheral bus; required for valid RBR/THR
     *   bit 2 (RESETTF) = 1 — reset TX FIFO
     *   bit 1 (RESETRF) = 1 — reset RX FIFO, clears stale data from U-Boot
     *   bit 0 (TRFIFOE) = 1 — enable FIFO (must be set before other bits)
     *   bits 7:6 (ITL)  = 0 — trigger level = 1 byte
     */
    *UART_FCR = (1u << 5) | 0x07;   /* = 0x27 */
}

#endif

#define LSR_DR     (1u << 0)   /* Data Ready: receive buffer has a byte */
#define LSR_THRE   (1u << 5)   /* TDRQ: TX holding register ready */

char uart_getc(void) {
    while (!(*UART_LSR & LSR_DR))
        ;
    char c = (char)(*UART_RBR & 0xFF);
    return (c == '\r') ? '\n' : c;
}

void uart_putc(char c) {
    if (c == '\n') {
        while (!(*UART_LSR & LSR_THRE))
            ;
        *UART_THR = '\r';
    }
    while (!(*UART_LSR & LSR_THRE))
        ;
    *UART_THR = (unsigned int)(unsigned char)c;
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
