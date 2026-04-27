/*
 * trap.c - C-level trap dispatcher.
 *
 * Called from trapasm.S with a pointer to the saved trap frame.
 */

#include "trap.h"
#include "plic.h"
#include "timer.h"

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putc(char c);
extern void uart_putdec(unsigned long n);
extern void uart_irq_handler(void);

/* ==== Advanced Exercise 2: Priority task queue with preemption ==== */

struct task_entry {
    task_callback_t callback;
    void           *arg;
    int             priority;  /* higher value = higher priority */
    int             active;
};

#define MAX_TASKS 32

static struct task_entry task_pool[MAX_TASKS];
static int task_count = 0;
static volatile int running_priority = -1; /* priority of currently executing task */

void add_task(task_callback_t callback, void *arg, int priority) {
    if (!callback || task_count >= MAX_TASKS) return;

    /* Insert sorted by priority (ascending: lowest first, highest last) */
    int pos = task_count;
    for (int i = 0; i < task_count; i++) {
        if (priority < task_pool[i].priority) {
            pos = i;
            break;
        }
    }
    /* Shift right */
    for (int i = task_count; i > pos; i--)
        task_pool[i] = task_pool[i - 1];

    task_pool[pos].callback = callback;
    task_pool[pos].arg = arg;
    task_pool[pos].priority = priority;
    task_pool[pos].active = 1;
    task_count++;
}

/*
 * Execute pending tasks with interrupts enabled (nested interrupts allowed).
 * Higher-priority tasks preempt lower-priority ones via running_priority check.
 */
static void process_pending_tasks(void) {
    while (1) {
        /* Find highest priority task (last in sorted array) */
        int idx = -1;
        for (int i = task_count - 1; i >= 0; i--) {
            if (task_pool[i].active && task_pool[i].priority > running_priority) {
                idx = i;
                break;
            }
        }
        if (idx < 0) break;

        /* Remove from queue */
        struct task_entry t = task_pool[idx];
        task_pool[idx].active = 0;
        /* Compact by right Shift elements left */
        for (int i = idx; i < task_count - 1; i++)
            task_pool[i] = task_pool[i + 1];
        task_count--;

        /* Execute with interrupts enabled (allows preemption) */
        int prev_priority = running_priority;
        running_priority = t.priority;
        asm volatile("csrsi sstatus, 2"); /* enable SIE */

        t.callback(t.arg);

        asm volatile("csrci sstatus, 2"); /* disable SIE */
        running_priority = prev_priority;
    }
}

/* ==== Syscall (ecall from U-mode) handler ==== */

extern void exec_return_to_kernel(void);

static void handle_ecall(struct trap_frame *tf) {
    unsigned long syscall_nr = tf->a7;

    /* Print diagnostic info */
    uart_puts("=== S-Mode trap ===\r\n");
    uart_puts("scause: ");
    uart_putdec(tf->scause & ~SCAUSE_INTERRUPT);
    uart_puts("\r\nsepc: ");
    uart_hex(tf->sepc);
    uart_puts("\r\nstval: ");
    uart_putdec(tf->stval);
    uart_puts("\r\n");

    switch (syscall_nr) {
    case 0: /* exit */
        uart_puts("[syscall] exit\r\n");
        exec_return_to_kernel();
        break;
    case 1: /* putchar(a0) */
        uart_putc((char)tf->a0);
        break;
    case 2: /* getchar -> a0 (non-blocking in trap context) */
        {
            extern int uart_async_read_ready(void);
            extern char uart_async_getc(void);
            if (uart_async_read_ready())
                tf->a0 = (unsigned long)(unsigned char)uart_async_getc();
            else
                tf->a0 = 0;
        }
        break;
    default:
        break;
    }

    /* Advance past the ecall instruction */
    tf->sepc += 4;
}

/* ==== External interrupt handler ==== */

static int pending_plic_irq = 0;

static void handle_external_irq(void) {
    int irq = plic_claim();
    if (irq == (int)plic_get_uart_irq()) {
        uart_irq_handler();
    } else if (irq != 0) {
        uart_puts("[plic] unknown IRQ: ");
        uart_hex((unsigned long)irq);
        uart_puts("\r\n");
    }
    /* Delay plic_complete until after process_pending_tasks,
     * so the device stays masked during bottom-half processing. */
    pending_plic_irq = irq;
}

/* ==== Main trap dispatcher ==== */

void do_trap(struct trap_frame *tf) {
    unsigned long scause = tf->scause;
    int is_interrupt = (scause >> 63) & 1;
    unsigned long code = scause & ~SCAUSE_INTERRUPT;

    pending_plic_irq = 0;

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
            while (1)
                ;
            break;
        }
    }

    /* Bottom half: run deferred tasks with interrupts enabled */
    process_pending_tasks();

    /* Unmask the device after tasks are done */
    if (pending_plic_irq)
        plic_complete(pending_plic_irq);
}
