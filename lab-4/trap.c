/*
 * trap.c - C-level trap dispatcher.
 *
 * Called from trap.S with a pointer to the saved trap frame.
 */

#include "trap.h"
#include "plic.h"
#include "timer.h"

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putc(char c);
extern void uart_irq_handler(void);

/* ---- Bottom-half task queue (Advanced Exercise 2) ---- */

typedef void (*task_fn_t)(void);

#define MAX_TASKS 32

static task_fn_t task_queue[MAX_TASKS];
static volatile int task_head = 0;
static volatile int task_tail = 0;

void enqueue_task(task_fn_t fn) {
    int next = (task_head + 1) % MAX_TASKS;
    if (next != task_tail) {
        task_queue[task_head] = fn;
        task_head = next;
    }
}

static void process_pending_tasks(void) {
    /* Only run if there are tasks queued (avoid unnecessary SIE toggling) */
    if (task_tail == task_head)
        return;

    /* Re-enable interrupts while processing bottom-half tasks */
    asm volatile("csrsi sstatus, 2"); /* set SIE */

    while (task_tail != task_head) {
        task_fn_t fn = task_queue[task_tail];
        task_tail = (task_tail + 1) % MAX_TASKS;
        fn();
    }

    /* Disable interrupts before returning to trap handler */
    asm volatile("csrci sstatus, 2"); /* clear SIE */
}

/* ---- Syscall (ecall from U-mode) handler ---- */

extern void exec_return_to_kernel(void);

static void handle_ecall(struct trap_frame *tf) {
    unsigned long syscall_nr = tf->a7;

    /* Print diagnostic info for non-trivial syscalls */
    if (syscall_nr != 1) {
        uart_puts("[ecall] scause=");
        uart_hex(tf->scause);
        uart_puts(" sepc=");
        uart_hex(tf->sepc);
        uart_puts(" stval=");
        uart_hex(tf->stval);
        uart_puts("\r\n");
    }

    switch (syscall_nr) {
    case 0: /* exit */
        uart_puts("[syscall] exit\r\n");
        /* longjmp back to kernel (caller of exec_save_and_switch) */
        exec_return_to_kernel();
        /* not reached */
        break;
    case 1: /* putchar(a0) */
        uart_putc((char)tf->a0);
        break;
    case 2: /* getchar -> a0 */
        {
            extern char uart_getc(void);
            tf->a0 = (unsigned long)(unsigned char)uart_getc();
        }
        break;
    default:
        uart_puts("[syscall] unknown syscall: ");
        uart_hex(syscall_nr);
        uart_puts("\r\n");
        break;
    }

    /* Advance past the ecall instruction */
    tf->sepc += 4;
}

/* ---- External interrupt handler ---- */

static void handle_external_irq(void) {
    int irq = plic_claim();
    if (irq == UART_IRQ) {
        uart_irq_handler();
    } else if (irq != 0) {
        uart_puts("[plic] unknown IRQ: ");
        uart_hex((unsigned long)irq);
        uart_puts("\r\n");
    }
    if (irq)
        plic_complete(irq);
}

/* ---- Main trap dispatcher ---- */

void do_trap(struct trap_frame *tf) {
    unsigned long scause = tf->scause;
    int is_interrupt = (scause >> 63) & 1;
    unsigned long code = scause & ~SCAUSE_INTERRUPT;

    uart_puts("[trap] scause=");
    uart_hex(scause);
    uart_puts(" sepc=");
    uart_hex(tf->sepc);
    uart_puts("\r\n");

    if (is_interrupt) {
        switch (code) {
        case IRQ_S_TIMER:
            handle_timer_irq();
            break;
        case IRQ_S_EXTERNAL:
            handle_external_irq();
            break;
        default:
            uart_puts("[trap] unknown interrupt: scause=");
            uart_hex(scause);
            uart_puts("\r\n");
            while (1);
            break;
        }
    } else {
        switch (code) {
        case EXC_ECALL_U:
            handle_ecall(tf);
            break;
        default:
            uart_puts("[trap] exception: scause=");
            uart_hex(scause);
            uart_puts(" sepc=");
            uart_hex(tf->sepc);
            uart_puts(" stval=");
            uart_hex(tf->stval);
            uart_puts("\r\n");
            /* Hang on unhandled exception */
            while (1)
                ;
            break;
        }
    }

}
