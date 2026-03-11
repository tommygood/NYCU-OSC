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

#ifdef QEMU

#define UART_RBR   ((volatile unsigned char *)(0x10000000UL + 0x00))
#define UART_THR   ((volatile unsigned char *)(0x10000000UL + 0x00))
#define UART_FCR   ((volatile unsigned char *)(0x10000000UL + 0x02))
#define UART_LSR   ((volatile unsigned char *)(0x10000000UL + 0x05))

void uart_init(void) {
    *UART_FCR = 0x07;   /* enable + reset FIFOs, trigger = 1 byte */
}

char uart_getc(void) {
    while (!(*UART_LSR & 0x01))
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
#define UART_IER   ((volatile unsigned int *)(0xD4017000UL + 0x04))
#define UART_FCR   ((volatile unsigned int *)(0xD4017000UL + 0x08))
#define UART_LCR   ((volatile unsigned int *)(0xD4017000UL + 0x0C))
#define UART_MCR   ((volatile unsigned int *)(0xD4017000UL + 0x10))
#define UART_LSR   ((volatile unsigned int *)(0xD4017000UL + 0x14))

/* Pinctrl registers for UART0 (uart0_2_grp from DTB):
 *   pinctrl base:  0xD401E000
 *   offset 0x114:  UART0_TX pin
 *   offset 0x118:  UART0_RX pin
 *   formula: new = (old & ~0xd040) | 0x2002
 *     bits[1:0]=2 → function 2 (UART0), bit13 → peripheral-function enable
 * TX is pre-configured by OpenSBI; RX must be set here. */
#define PINCTRL_BASE  0xD401E000UL
#define PIN_UART0_TX  ((volatile unsigned int *)(PINCTRL_BASE + 0x114))
#define PIN_UART0_RX  ((volatile unsigned int *)(PINCTRL_BASE + 0x118))
#define PIN_FUNC_UART0  0x2002u  /* fn=2 (bits[1:0]) + bit13 (peripheral enable) */
#define PIN_CONF_MASK   0xd040u

void uart_init(void) {
    /* Set TX and RX pins to function 2 (UART0) with peripheral-function enable. */
    *PIN_UART0_TX = (*PIN_UART0_TX & ~PIN_CONF_MASK) | PIN_FUNC_UART0;
    *PIN_UART0_RX = (*PIN_UART0_RX & ~PIN_CONF_MASK) | PIN_FUNC_UART0;

    *UART_LCR = 0x03;                          /* 8N1, DLAB=0 */
    *UART_MCR = 0x03;                          /* DTR=1, RTS=1, LOOP=0 */
    *UART_IER = (1u << 6);                     /* UUE=1 */
    *UART_FCR = (1u << 5) | (1u << 1) | 0x01; /* BUS=1, RESETRF=1, TRFIFOE=1 */

    /* Drain any bytes OpenSBI left in the RX FIFO. */
    while (*UART_LSR & 0x01)
        (void)*UART_RBR;
}

char uart_getc(void) {
    while (!(*UART_LSR & 0x01))
        ;
    char c = (char)(*UART_RBR & 0xFF);
    return (c == '\r') ? '\n' : c;
}

#endif  /* QEMU / real board */

/* ── common TX path (direct MMIO works from S-mode on K1) ─────────────── */

#define LSR_THRE   (1u << 5)   /* TDRQ: TX holding register ready */

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
