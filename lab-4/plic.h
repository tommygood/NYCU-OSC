/*
 * plic.h - Platform-Level Interrupt Controller
 */
#ifndef PLIC_H
#define PLIC_H

void plic_set_base(unsigned long base);
void plic_set_uart_irq(unsigned long irq);
unsigned long plic_get_uart_irq(void);
void plic_init(void);
int  plic_claim(void);
void plic_complete(int irq);

#endif /* PLIC_H */
