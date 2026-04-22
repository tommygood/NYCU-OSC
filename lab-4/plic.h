/*
 * plic.h - Platform-Level Interrupt Controller
 */
#ifndef PLIC_H
#define PLIC_H

#ifdef QEMU
#define UART_IRQ  10    /* QEMU virt */
#else
#define UART_IRQ  42    /* OrangePi RV2 / SpacemiT K1 (0x2a from DTB) */
#endif

void plic_init(void);
int  plic_claim(void);
void plic_complete(int irq);

#endif /* PLIC_H */
