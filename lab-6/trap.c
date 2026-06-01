/*
 * trap.c - C-level trap dispatcher with syscall handling.
 */

#include "trap.h"
#include "plic.h"
#include "timer.h"
#include "thread.h"

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putc(char c);
extern char uart_getc(void);
extern void uart_putdec(unsigned long n);
extern void uart_irq_handler(void);
extern int  uart_async_read_ready(void);
extern char uart_async_getc(void);

/* ==== Priority task queue with preemption ==== */

struct task_entry {
    task_callback_t callback;
    void           *arg;
    int             priority;
    int             active;
};

#define MAX_TASKS 32

static struct task_entry task_pool[MAX_TASKS];
static int task_count = 0;
static volatile int running_priority = -1;

void add_task(task_callback_t callback, void *arg, int priority) {
    if (!callback || task_count >= MAX_TASKS) return;

    int pos = task_count;
    for (int i = 0; i < task_count; i++) {
        if (priority < task_pool[i].priority) {
            pos = i;
            break;
        }
    }
    for (int i = task_count; i > pos; i--)
        task_pool[i] = task_pool[i - 1];

    task_pool[pos].callback = callback;
    task_pool[pos].arg = arg;
    task_pool[pos].priority = priority;
    task_pool[pos].active = 1;
    task_count++;
}

static void process_pending_tasks(void) {
    // disable interrupt before accessing shared task queue
    asm volatile("csrci sstatus, 2");

    while (1) {
        int idx = -1;
        for (int i = task_count - 1; i >= 0; i--) {
            if (task_pool[i].active && task_pool[i].priority > running_priority) {
                idx = i;
                break;
            }
        }
        if (idx < 0) break;

        struct task_entry t = task_pool[idx];
        task_pool[idx].active = 0;
        for (int i = idx; i < task_count - 1; i++)
            task_pool[i] = task_pool[i + 1];
        task_count--;

        int prev_priority = running_priority;
        running_priority = t.priority;
        asm volatile("csrsi sstatus, 2");

        t.callback(t.arg);

        asm volatile("csrci sstatus, 2");
        running_priority = prev_priority;
    }
}

/* ==== Syscall handler ==== */

extern unsigned long k_strlen(const char *s);

static void handle_ecall(struct trap_frame *tf) {
    unsigned long syscall_nr = tf->a7;

    /* Enable interrupts so UART TX/RX and timer work during syscalls */
    asm volatile("csrsi sstatus, 2");


    switch (syscall_nr) {
    case 0: /* getpid() */
        tf->a0 = (unsigned long)sys_getpid();
        break;

    case 1: /* uart_read(buf, count) */
        {
            char *buf = (char *)tf->a0;
            long count = (long)tf->a1;
            long i = 0;
            for (i = 0; i < count; i++) {
                buf[i] = uart_async_getc();
            }
            tf->a0 = (unsigned long)i;
        }
        break;

    case 2: /* uart_write(buf, count) */
        {
            const char *buf = (const char *)tf->a0;
            long count = (long)tf->a1;
            for (long i = 0; i < count; i++) {
                if (buf[i] == '\n') uart_putc('\r');
                uart_putc(buf[i]);
            }
            tf->a0 = (unsigned long)count;
        }
        break;

    case 3: /* exec(path) */
        {
            const char *path = (const char *)tf->a0;
            tf->a0 = (unsigned long)sys_exec(path, tf);
            if ((int)tf->a0 == 0) {
                /* exec succeeded; sepc and sp already updated by sys_exec */
                /* Advance past ecall not needed since we set sepc directly */
                return;
            }
            else {
                uart_puts("[syscall] exec failed: ");
            }
        }
        break;

    case 4: /* fork() */
        tf->a0 = (unsigned long)sys_fork(tf);
        break;

    case 5: /* waitpid(pid) */
        tf->a0 = (unsigned long)sys_waitpid((long)tf->a0);
        break;

    case 6: /* exit(status) */
        sys_exit((int)tf->a0);
        /* Should not return */
        break;

    case 7: /* stop(pid) */
        tf->a0 = (unsigned long)sys_stop((long)tf->a0);
        break;

    case 8: /* display(bmp_image, width, height) */
        {
            extern void video_display(unsigned int *bmp, unsigned int w, unsigned int h);
            video_display((unsigned int *)tf->a0,
                          (unsigned int)tf->a1,
                          (unsigned int)tf->a2);
            tf->a0 = 0;
        }
        break;

    case 9: /* usleep(usec) */
        {
            unsigned int usec = (unsigned int)tf->a0;
            extern void sys_usleep(unsigned long usec);
            sys_usleep((unsigned long)usec);
            tf->a0 = 0;
        }
        break;

    case 10: /* signal(signum, handler) */
        tf->a0 = sys_signal((int)tf->a0, (void (*)())tf->a1);
        break;

    case 11: /* sigreturn() */
        sys_sigreturn(tf);
        return; /* don't advance sepc — restored from saved context */

    case 12: /* kill(pid, signum) */
        tf->a0 = (unsigned long)sys_kill((int)tf->a0, (int)tf->a1);
        break;

    case 13: /* mmap(addr, length, prot, flags) */
        tf->a0 = sys_mmap(tf->a0, tf->a1, (int)tf->a2, (int)tf->a3);
        break;

    default:
        uart_puts("[syscall] unknown: ");
        uart_putdec(syscall_nr);
        uart_puts("\r\n");
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
    pending_plic_irq = irq;
}

/* ==== Timer interrupt: trigger preemptive scheduling ==== */

/* ==== Main trap dispatcher ==== */

void do_trap(struct trap_frame *tf) {
    unsigned long scause = tf->scause;
    int is_interrupt = (scause >> 63) & 1;
    unsigned long code = scause & ~SCAUSE_INTERRUPT;

    pending_plic_irq = 0;

    if (is_interrupt) {
        switch (code) {
        case IRQ_S_TIMER:
        {
            static volatile int in_timer_schedule = 0;
            handle_timer_irq();
            if (!in_timer_schedule) {
                in_timer_schedule = 1;
                schedule();
                /* Reset even if we switched to a different thread —
                 * the thread that resumes here might not be the one
                 * that set the flag. */
            }
            in_timer_schedule = 0;
            break;
        }
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
        case 12: /* Instruction page fault */
        case 13: /* Load page fault */
        case 15: /* Store/AMO page fault */
            /* User-mode page fault */
            if (!(tf->sstatus & SSTATUS_SPP)) {
                /* Try demand paging — check if fault addr is in a valid VMA */
                if (handle_page_fault(tf->stval)) {
                    break;  /* page allocated, resume user process */
                }
                /* Not in any VMA → segmentation fault */
                uart_puts("[Segmentation fault]: Kill Process ");
                uart_putdec((unsigned long)get_current()->pid);
                uart_puts(" at ");
                uart_hex(tf->stval);
                uart_puts("\r\n");
                thread_exit();
                while (1);
            }
            /* Fall through for S-mode faults */
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

    /* Bottom half: run deferred tasks (manages SIE internally) */
    process_pending_tasks();

    /* Unmask the device after tasks are done */
    if (pending_plic_irq)
        plic_complete(pending_plic_irq);

    /* Check for pending signals before returning to user mode */
    check_pending_signal(tf);
}
