/*
 * plic.c - Platform-Level Interrupt Controller driver.
 *
 * S-mode context = 2 * hart_id + 1.
 */

static unsigned long plic_base = 0x0c000000UL;  /* default QEMU */
static unsigned long uart_irq = 10;             /* default QEMU */

/* Per-IRQ priority register (IRQ 0 is reserved) */
#define PLIC_PRIORITY(irq)      (plic_base + (irq) * 4)

/* Per-context enable bits: base + 0x2000 + context * 0x80 */
#define PLIC_ENABLE(ctx)        (plic_base + 0x2000UL + (ctx) * 0x80UL)

/* Per-context threshold: base + 0x200000 + context * 0x1000 */
#define PLIC_THRESHOLD(ctx)     (plic_base + 0x200000UL + (ctx) * 0x1000UL)

/* Per-context claim/complete: base + 0x200004 + context * 0x1000 */
#define PLIC_CLAIM(ctx)         (plic_base + 0x200004UL + (ctx) * 0x1000UL)

void plic_set_base(unsigned long base) {
    plic_base = base;
}

void plic_set_uart_irq(unsigned long irq) {
    uart_irq = irq;
}

unsigned long plic_get_uart_irq(void) {
    return uart_irq;
}

#include "plic.h"

extern unsigned long saved_hart_id; // Set in start.S
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putdec(unsigned long n);

static unsigned long plic_ctx(void) {
    return 2 * saved_hart_id + 1;
}

void plic_init(void) {
    unsigned long ctx = plic_ctx();

    /* Set UART interrupt priority to 1 */
    *(volatile unsigned int *)PLIC_PRIORITY(uart_irq) = 1;

    /* Enable UART IRQ for this context.
     * Enable registers are 32-bit words; IRQ N is bit (N%32) in word (N/32). */
    unsigned long enable_addr = PLIC_ENABLE(ctx) + (uart_irq / 32) * 4;
    *(volatile unsigned int *)enable_addr = (1U << (uart_irq % 32));

    /* Set priority threshold to 0 (allow all priorities) */
    *(volatile unsigned int *)PLIC_THRESHOLD(ctx) = 0;

    uart_puts("[plic] ctx=");
    uart_putdec(ctx);
    uart_puts(" IRQ=");
    uart_putdec(uart_irq);
    uart_puts("\r\n");
}

int plic_claim(void) {
    /* return 0 if there's no pending interrupt, otherwise highest priority IRQ */ 
    return *(volatile unsigned int *)PLIC_CLAIM(plic_ctx());
}

void plic_complete(int irq) {
    /* This unlocks that IRQ source so the PLIC can deliver it again next time it fires. */
    *(volatile unsigned int *)PLIC_CLAIM(plic_ctx()) = irq;
}
