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
 * TX is done via direct MMIO (works fine from S-mode on K1).
 * RX is done via SBI legacy console getchar (EID=0x01): OpenSBI owns
 * the UART in M-mode and the DR flag is never set in S-mode direct access.
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

void uart_debug_regs(void) {}      /* no-op in QEMU build */
void uart_loopback_test(void) {}   /* no-op in QEMU build */

char uart_getc(void) {
    while (!(*UART_LSR & 0x01))
        ;
    char c = (char)(*UART_RBR & 0xFF);
    return (c == '\r') ? '\n' : c;
}

#else  /* real board: OrangePi RV2 / SpacemiT K1 */

/* forward declarations — defined in the common section below */
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);

/* UART_2 (DTB serial0, K1 hw UART_2): used for TX */
#define UART_RBR   ((volatile unsigned int *)(0xD4017000UL + 0x00))
#define UART_THR   ((volatile unsigned int *)(0xD4017000UL + 0x00))
#define UART_IER   ((volatile unsigned int *)(0xD4017000UL + 0x04))
#define UART_FCR   ((volatile unsigned int *)(0xD4017000UL + 0x08))
#define UART_LCR   ((volatile unsigned int *)(0xD4017000UL + 0x0C))
#define UART_MCR   ((volatile unsigned int *)(0xD4017000UL + 0x10))
#define UART_LSR   ((volatile unsigned int *)(0xD4017000UL + 0x14))

/* UART2 at 0xD4017100 (address map: UART0=0xD4017000, UART2=0xD4017100, no UART1 in main domain).
 * The pinctrl group "uart0_2_grp" may route RX (pin 0x118, fn=2) to UART2, not UART0. */
#define UART2_RBR  ((volatile unsigned int *)(0xD4017100UL + 0x00))
#define UART2_LSR  ((volatile unsigned int *)(0xD4017100UL + 0x14))

/* SEC UART1 at 0xF0612000 (secure domain, separate from main UART0) */
#define UART0_RBR  ((volatile unsigned int *)(0xF0612000UL + 0x00))
#define UART0_LSR  ((volatile unsigned int *)(0xF0612000UL + 0x14))

/* Pinctrl registers for UART0 (uart0_2_grp from DTB):
 *   pinctrl base:  0xD401E000
 *   offset 0x114:  UART0_TX pin
 *   offset 0x118:  UART0_RX pin
 *   formula: new = (old & ~0xd040) | 0x02  (function 2 = UART0)
 *
 * U-Boot uses SBI ecalls for console input, so it never configures the
 * RX pinmux.  The RX pin is left in GPIO mode (function 0) until we
 * set it here.  TX works because OpenSBI configures it for its own TX. */
#define PINCTRL_BASE  0xD401E000UL
#define PIN_UART0_TX  ((volatile unsigned int *)(PINCTRL_BASE + 0x114))
#define PIN_UART0_RX  ((volatile unsigned int *)(PINCTRL_BASE + 0x118))
#define PIN_FUNC_UART0  0x2002u  /* fn=2 (bits[1:0]) + bit13 (peripheral enable) */
#define PIN_CONF_MASK   0xd040u

void uart_init(void) {
    /* Configure UART0 TX and RX pins.
     * TX was already 0x2002 (fn=2 + bit13).  RX was 0x0000 — we previously
     * set fn=2 but missed bit13 (peripheral-function enable).  Without bit13
     * the function field in bits[1:0] is ignored and the pin stays in GPIO
     * mode, so UART RX circuit never sees external signals. */
    *PIN_UART0_TX = (*PIN_UART0_TX & ~PIN_CONF_MASK) | PIN_FUNC_UART0;
    *PIN_UART0_RX = (*PIN_UART0_RX & ~PIN_CONF_MASK) | PIN_FUNC_UART0;

    *UART_LCR = 0x03;               /* 8N1, DLAB=0 */
    *UART_MCR = 0x03;               /* DTR=1, RTS=1, LOOP=0 */
    *UART_IER = (1u << 6);          /* UUE=1 */
    *UART_FCR = (1u << 5) | 0x01;  /* BUS=1, TRFIFOE=1 */
}

void uart_debug_regs(void) {
    uart_puts("[pinctrl] TX="); uart_hex(*PIN_UART0_TX);
    uart_puts(" RX=");          uart_hex(*PIN_UART0_RX);
    uart_puts("\r\n");
    uart_puts("[UART0@D4017000] IER="); uart_hex(*UART_IER);
    uart_puts(" LCR=");       uart_hex(*UART_LCR);
    uart_puts(" MCR=");       uart_hex(*UART_MCR);
    uart_puts(" LSR=");       uart_hex(*UART_LSR);
    uart_puts("\r\n");
    uart_puts("[UART2@D4017100] LSR="); uart_hex(*UART2_LSR);
    uart_puts("\r\n");
    uart_puts("[SecUART@F0612000] LSR="); uart_hex(*UART0_LSR);
    uart_puts("\r\n");
}

/*
 * uart_loopback_test: set MCR.LOOP=1 to internally connect TX→RX.
 * If the UART chip receives what it sends, the hardware is fine.
 * If it fails, there is a deeper hardware or configuration problem.
 */
void uart_loopback_test(void) {
    int ok = 0;

    /* Enable hardware loopback: TX pin disconnected, TX→RX internally */
    *UART_MCR = (1u << 4) | 0x03;  /* LOOP=1, RTS=1, DTR=1 */

    /* Drain any stale RX data */
    while (*UART_LSR & 0x01)
        (void)*UART_RBR;

    /* Send test byte 0x55 */
    while (!(*UART_LSR & (1u << 5))) ;
    *UART_THR = 0x55;

    /* Wait for it to loop back (~1 byte at 115200 = 87 µs) */
    unsigned long lim = 2000000UL;
    while (!(*UART_LSR & 0x01) && --lim) ;

    if (lim && (*UART_LSR & 0x01)) {
        (void)*UART_RBR;
        ok = 1;
    }

    /* Disable loopback BEFORE printing so output doesn't re-enter RX */
    *UART_MCR = 0x03;

    if (ok)
        uart_puts("[loopback OK — UART RX chip works; check TX/RX wiring]\r\n");
    else
        uart_puts("[loopback FAIL — UART RX hardware or config broken]\r\n");
}

/*
 * uart_getc: poll both UART_2 (0xD4017000) and UART_0 (0xF0612000).
 *
 * Per the K1 manual, UART_0 is a separate hardware unit at 0xF0612000.
 * The physical RX pin on the debug header may route to UART_0 rather than
 * UART_2, while TX from UART_2 drives the physical TX pin.  We poll both
 * LSR.DR bits so whichever UART actually receives the character is read.
 */
char uart_getc(void) {
    while (1) {
        if (*UART_LSR & 0x01) {        /* UART_2 has data */
            char c = (char)(*UART_RBR & 0xFF);
            return (c == '\r') ? '\n' : c;
        }
        if (*UART2_LSR & 0x01) {       /* UART2@D4017100 has data */
            char c = (char)(*UART2_RBR & 0xFF);
            return (c == '\r') ? '\n' : c;
        }
        if (*UART0_LSR & 0x01) {       /* SecUART@F0612000 has data */
            char c = (char)(*UART0_RBR & 0xFF);
            return (c == '\r') ? '\n' : c;
        }
    }
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
