/*
 * UART driver for OrangePi RV2 (SpacemiT K1) and QEMU virt.
 *
 * Provides both synchronous (polling) and asynchronous (interrupt-driven) I/O.
 */

#define LSR_DR   (1u << 0)   /* data ready  (RX) */
#define LSR_THRE (1u << 5)   /* TX holding register empty / TDRQ */

static unsigned long uart_base = 0;

void uart_set_base(unsigned long base) {
    uart_base = base;
}

unsigned long uart_debug_get_base(void) {
    return uart_base;
}

/* ---- Circular buffer for async I/O ---- */

#define BUF_SIZE 256

struct ring_buf {
    char data[BUF_SIZE];
    volatile int head;
    volatile int tail;
};

static struct ring_buf rx_buf = { .head = 0, .tail = 0 };
static struct ring_buf tx_buf = { .head = 0, .tail = 0 };

static int buf_empty(struct ring_buf *b) {
    return b->head == b->tail;
}

static int buf_full(struct ring_buf *b) {
    return ((b->head + 1) % BUF_SIZE) == b->tail;
}

static void buf_push(struct ring_buf *b, char c) {
    if (!buf_full(b)) {
        b->data[b->head] = c;
        b->head = (b->head + 1) % BUF_SIZE;
    }
}

static char buf_pop(struct ring_buf *b) {
    if (buf_empty(b)) return 0;
    char c = b->data[b->tail];
    b->tail = (b->tail + 1) % BUF_SIZE;
    return c;
}

/* Flag: 1 when interrupt-driven UART is active */
static int uart_async_enabled = 0;

#ifdef QEMU

/* ---- QEMU NS16550A ---- */

#define UART_THR(base)  ((volatile unsigned char *)((base) + 0x00))
#define UART_RBR(base)  ((volatile unsigned char *)((base) + 0x00))
#define UART_IER(base)  ((volatile unsigned char *)((base) + 0x01))
#define UART_IIR(base)  ((volatile unsigned char *)((base) + 0x02))
#define UART_FCR(base)  ((volatile unsigned char *)((base) + 0x02))
#define UART_LCR(base)  ((volatile unsigned char *)((base) + 0x03))
#define UART_MCR(base)  ((volatile unsigned char *)((base) + 0x04))
#define UART_LSR(base)  ((volatile unsigned char *)((base) + 0x05))

char uart_getc(void) {
    volatile unsigned char *lsr = UART_LSR(uart_base);
    volatile unsigned char *rbr = UART_RBR(uart_base);
    while (!(*lsr & LSR_DR))
        ;
    return *rbr;
}

void uart_putc(char c) {
    volatile unsigned char *lsr = UART_LSR(uart_base);
    volatile unsigned char *thr = UART_THR(uart_base);
    while (!(*lsr & LSR_THRE))
        ;
    *thr = (unsigned char)c;
}

void uart_flush_rx(void) {
    volatile unsigned char *lsr = UART_LSR(uart_base);
    volatile unsigned char *rbr = UART_RBR(uart_base);
    while (*lsr & LSR_DR) (void)*rbr;
}

void uart_init(void) {
    volatile unsigned char *ier = UART_IER(uart_base);
    volatile unsigned char *fcr = UART_FCR(uart_base);
    volatile unsigned char *lcr = UART_LCR(uart_base);
    *ier = 0x00;
    *lcr = 0x03;
    *fcr = 0x07;
}

/* Enable UART RX interrupt (and optionally TX) */
void uart_enable_irq(void) {
    /* IER bit 0: RX data available interrupt */
    *UART_IER(uart_base) = 0x01;
    uart_async_enabled = 1;
}

/* Enable TX interrupt */
static void uart_enable_tx_irq(void) {
    *UART_IER(uart_base) = 0x03; /* bit 0: RX, bit 1: TX */
}

/* Disable TX interrupt */
static void uart_disable_tx_irq(void) {
    *UART_IER(uart_base) = 0x01; /* RX only */
}

/* UART ISR - called from PLIC handler */
void uart_irq_handler(void) {
    unsigned char iir = *UART_IIR(uart_base);

    /* IIR bit 0 = 0 means interrupt pending */
    while (!(iir & 0x01)) {
        unsigned char id = (iir >> 1) & 0x07;

        if (id == 0x02 || id == 0x06) {
            /* RX data available (0x02) or character timeout (0x06) */
            while (*UART_LSR(uart_base) & LSR_DR) {
                char c = *UART_RBR(uart_base);
                buf_push(&rx_buf, c);
            }
        } else if (id == 0x01) {
            /* TX holding register empty */
            if (!buf_empty(&tx_buf)) {
                *UART_THR(uart_base) = (unsigned char)buf_pop(&tx_buf);
            } else {
                uart_disable_tx_irq();
            }
        }

        iir = *UART_IIR(uart_base);
    }
}

#else  /* OrangePi RV2 / SpacemiT K1 */

#define UART_THR_K1(base)  ((volatile unsigned int *)((base) + 0x00))
#define UART_RBR_K1(base)  ((volatile unsigned int *)((base) + 0x00))
#define UART_IER_K1(base)  ((volatile unsigned int *)((base) + 0x04))
#define UART_IIR_K1(base)  ((volatile unsigned int *)((base) + 0x08))
#define UART_FCR_K1(base)  ((volatile unsigned int *)((base) + 0x08))
#define UART_LCR_K1(base)  ((volatile unsigned int *)((base) + 0x0C))
#define UART_LSR_K1(base)  ((volatile unsigned int *)((base) + 0x14))

char uart_getc(void) {
    volatile unsigned int *lsr = UART_LSR_K1(uart_base);
    volatile unsigned int *rbr = UART_RBR_K1(uart_base);
    while (!(*lsr & LSR_DR))
        ;
    return (char)(*rbr & 0xFF);
}

void uart_putc(char c) {
    volatile unsigned int *lsr = UART_LSR_K1(uart_base);
    volatile unsigned int *thr = UART_THR_K1(uart_base);
    while (!(*lsr & LSR_THRE))
        ;
    *thr = (unsigned char)c;
}

void uart_flush_rx(void) {
    volatile unsigned int *lsr = UART_LSR_K1(uart_base);
    volatile unsigned int *rbr = UART_RBR_K1(uart_base);
    while (*lsr & LSR_DR) (void)*rbr;
}

void uart_init(void) {
    volatile unsigned int *ier = UART_IER_K1(uart_base);
    volatile unsigned int *fcr = UART_FCR_K1(uart_base);
    volatile unsigned int *lcr = UART_LCR_K1(uart_base);
    *ier = (1u << 6);
    *fcr = (1u << 5) | (1u << 0);
    *lcr = 0x03;
}

void uart_enable_irq(void) {
    /* MCR OUT2 (bit 3): gates interrupt output on 16550-compatible UARTs */
    volatile unsigned int *mcr = (volatile unsigned int *)(uart_base + 0x10);
    *mcr = (1u << 3); // Setting Bit 3 (OUT2) to 1 allows the UART interrupt signal to be routed to the interrupt controller (PLIC).

    /* 
       Reset FIFOs before enable interrupt
       TRFIFOE (bit 0): enable the TX/RX FIFOs. Without FIFOs enabled, the UART has no buffer and interrupts won't work properly.
       RESETRF (bit 1): flush the RX FIFO
       RESETTF (bit 2): flush the TX FIFO
       BUS32 (bit 5): tell the UART controller to use 32-bit bus access (K1's pxa-uart requirement)
    */
    *UART_FCR_K1(uart_base) = (1u << 5) | (1u << 2) | (1u << 1) | (1u << 0);

    /* 
       UUE (bit 6): UART Unit Enable — the K1 pxa-uart won't function at all without this master enable bit
       RTOIE (bit 4): Receiver Timeout Interrupt Enable — fires when data sits in the FIFO for too long without being read (handles the case where fewer bytes than the trigger level arrive)                                                              RAVIE (bit 0): Receiver Data Available Interrupt Enable — fires when RX FIFO has data  
    */
    *UART_IER_K1(uart_base) = (1u << 6) | (1u << 4) | (1u << 0);
    uart_async_enabled = 1;
}

static void uart_enable_tx_irq(void) {
    *UART_IER_K1(uart_base) = (1u << 6) | (1u << 4) | (1u << 1) | (1u << 0);
}

static void uart_disable_tx_irq(void) {
    *UART_IER_K1(uart_base) = (1u << 6) | (1u << 4) | (1u << 0);
}

void uart_irq_handler(void) {
    unsigned int iir = *UART_IIR_K1(uart_base);

    while (!(iir & 0x01)) {
        unsigned int id = (iir >> 1) & 0x07;

        if (id == 0x02 || id == 0x06) {
            /* RX data available (0x02) or character timeout (0x06) */
            while (*UART_LSR_K1(uart_base) & LSR_DR) {
                char c = (char)(*UART_RBR_K1(uart_base) & 0xFF);
                buf_push(&rx_buf, c);
            }
        } else if (id == 0x01) {
            if (!buf_empty(&tx_buf)) {
                *UART_THR_K1(uart_base) = (unsigned char)buf_pop(&tx_buf);
            } else {
                uart_disable_tx_irq();
            }
        }

        iir = *UART_IIR_K1(uart_base);
    }
}

#endif  /* QEMU / K1 */

/* ---- Async (interrupt-driven) read/write API ---- */

char uart_async_getc(void) {
    /* Wait until data available in RX buffer */
    while (buf_empty(&rx_buf))
        asm volatile("wfi");  /* sleep until interrupt */
    return buf_pop(&rx_buf);
}

void uart_async_putc(char c) {
    /* Wait if TX buffer is full */
    while (buf_full(&tx_buf))
        ;
    buf_push(&tx_buf, c);
    uart_enable_tx_irq();
}

void uart_async_puts(const char *s) {
    while (*s)
        uart_async_putc(*s++);
}

int uart_async_read_ready(void) {
    return !buf_empty(&rx_buf);
}

/* ---- Common utilities ---- */

unsigned long uart_read_u32_le(void) {
    unsigned long r = 0;
    r |= (unsigned long)(unsigned char)uart_getc() <<  0;
    r |= (unsigned long)(unsigned char)uart_getc() <<  8;
    r |= (unsigned long)(unsigned char)uart_getc() << 16;
    r |= (unsigned long)(unsigned char)uart_getc() << 24;
    return r;
}

void uart_putdec(unsigned long n) {
    char buf[20];
    int i = 0;
    if (n == 0) { uart_putc('0'); return; }
    while (n) { buf[i++] = '0' + (int)(n % 10); n /= 10; }
    while (i--) uart_putc(buf[i]);
}

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
