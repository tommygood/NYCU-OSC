/*
 * thread.c - Thread creation, scheduling, and process management.
 */
#include "thread.h"
#include "mm.h"
#include "timer.h"

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putdec(unsigned long n);
extern int k_strcmp(const char *a, const char *b);

/* initrd helpers (defined in main.c) */
extern int initrd_find_file(const char *filename, const unsigned char **data_out, int *size_out);

int nr_threads = 0;
static struct task_struct *run_queue = 0;

/* ── Run queue (circular doubly-linked list) ─────────────────────────────── */

static void enqueue(struct task_struct *task) {
    if (!run_queue) {
        run_queue = task;
        task->next = task;
        task->prev = task;
    } else {
        /* Insert before run_queue (at the tail) */
        struct task_struct *tail = run_queue->prev;
        tail->next = task;
        task->prev = tail;
        task->next = run_queue;
        run_queue->prev = task;
    }
}

static void dequeue(struct task_struct *task) {
    if (task->next == task) {
        /* Only element */
        run_queue = 0;
    } else {
        task->prev->next = task->next;
        task->next->prev = task->prev;
        if (run_queue == task)
            run_queue = task->next;
    }
    task->next = task->prev = 0;
}

/* ── Get current thread via tp register ──────────────────────────────────── */

struct task_struct *get_current(void) {
    struct task_struct *current;
    asm volatile("mv %0, tp" : "=r"(current));
    return current;
}

/* ── Thread wrapper: calls fn, then thread_exit ──────────────────────────── */

static void thread_entry(void) {
    /* New threads may start from a trap handler where SIE=0 */
    asm volatile("csrsi sstatus, 2");
    struct task_struct *cur = get_current();
    cur->entry_fn();
    thread_exit();
}

/* ── Thread creation ─────────────────────────────────────────────────────── */

struct task_struct *thread_create(void (*fn)()) {
    struct task_struct *task = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!task) return 0;

    void *stack = allocate(STACK_SIZE);
    if (!stack) { free(task); return 0; }

    task->pid = nr_threads++;
    task->state = TASK_RUNNING;
    task->stack = (unsigned long)stack;
    task->kernel_sp = (unsigned long)stack + STACK_SIZE;
    task->user_sp = 0;
    task->user_stack = 0;
    task->prog = 0;
    task->prog_size = 0;
    task->tf = 0;
    task->wait_target = 0;
    task->entry_fn = fn;
    task->pending_timer = 0;
    for (int i = 0; i < MAX_SIG; i++) task->sig_handlers[i] = 0;
    task->pending_sig = -1;
    task->saved_tf = 0;
    task->sig_stack = 0;

    /* Set up context: ra = thread_entry, sp = top of stack */
    task->thread.ra = (unsigned long)thread_entry;
    task->thread.sp = task->kernel_sp;

    enqueue(task);
    return task;
}

/* ── Schedule (round-robin) ──────────────────────────────────────────────── */

void schedule(void) {
    struct task_struct *current = get_current();
    struct task_struct *next = current->next;

    /* Skip zombies and waiting tasks */
    while (next != current && (next->state == TASK_ZOMBIE || next->state == TASK_WAITING))
        next = next->next;

    if (next == current) return;  /* no other runnable thread */

    switch_to(current, next);

    /* Re-enable interrupts after resuming from switch_to.
     * We may have been resumed from within a trap handler (SIE=0). */
    asm volatile("csrsi sstatus, 2");
}

/* ── Kill zombies ────────────────────────────────────────────────────────── */

void kill_zombies(void) {
    if (!run_queue) return;

    struct task_struct *p = run_queue;
    struct task_struct *start = p;
    while (p) {
        struct task_struct *next = p->next;
        if (p->state == TASK_ZOMBIE && !p->pending_timer) {
            dequeue(p);
            if (p->stack) free((void *)p->stack);
            if (p->user_stack) free((void *)p->user_stack);
            if (p->prog) free((void *)p->prog);
            free(p);
        }
        p = next;
        if (p == start) break;
    }
}

/* ── Thread exit ─────────────────────────────────────────────────────────── */

void thread_exit(void) {
    struct task_struct *current = get_current();
    current->state = TASK_ZOMBIE;

    /* Wake up any task waiting on us */
    struct task_struct *p = run_queue;
    struct task_struct *start = p;
    while (p) {
        if (p->state == TASK_WAITING && p->wait_target == current) {
            p->state = TASK_RUNNING;
            p->wait_target = 0;
        }
        p = p->next;
        if (p == start) break;
    }

    schedule();
}

/* ── Idle thread ─────────────────────────────────────────────────────────── */

void idle(void) {
    while (1) {
        kill_zombies();
        schedule();
    }
}

/* ── System calls ────────────────────────────────────────────────────────── */

int sys_getpid(void) {
    return get_current()->pid;
}

extern void *k_memcpy(void *dst, const void *src, unsigned long n);

long sys_fork(struct trap_frame *parent_tf) {
    struct task_struct *parent = get_current();
    struct task_struct *child = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!child) return -1;

    /* Allocate kernel stack for child */
    void *kstack = allocate(STACK_SIZE);
    if (!kstack) { free(child); return -1; }

    /* Allocate user stack for child */
    void *ustack = allocate(USER_STACK_SIZE);
    if (!ustack) { free(kstack); free(child); return -1; }

    child->pid = nr_threads++;
    child->state = TASK_RUNNING;
    child->stack = (unsigned long)kstack;
    child->kernel_sp = (unsigned long)kstack + STACK_SIZE;
    child->user_stack = (unsigned long)ustack;
    /* Share parent's program (no MMU, same physical address) */
    child->prog = parent->prog;
    child->prog_size = parent->prog_size;
    child->wait_target = 0;
    child->pending_timer = 0;
    for (int i = 0; i < MAX_SIG; i++) child->sig_handlers[i] = parent->sig_handlers[i];
    child->pending_sig = -1;
    child->saved_tf = 0;
    child->sig_stack = 0;

    /* Copy parent's kernel stack */
    k_memcpy(kstack, (void *)parent->stack, STACK_SIZE);

    /* Calculate the trap frame offset in the new kernel stack */
    unsigned long tf_offset = (unsigned long)parent_tf - parent->stack;
    struct trap_frame *child_tf = (struct trap_frame *)((unsigned long)kstack + tf_offset);
    child->tf = child_tf;

    /* Child's fork returns 0, and set tp to child task */
    child_tf->a0 = 0;
    child_tf->tp = (unsigned long)child;
    child_tf->sepc += 4;  /* advance past ecall instruction */

    /* Adjust user sp: calculate offset from parent user stack base */
    if (parent->user_stack && parent_tf->sp) {
        unsigned long user_sp_offset = parent_tf->sp - parent->user_stack;
        child_tf->sp = (unsigned long)ustack + user_sp_offset;
        /* Copy user stack contents */
        k_memcpy(ustack, (void *)parent->user_stack, USER_STACK_SIZE);
    }

    /* No sepc relocation needed — child shares parent's program (no MMU) */

    /* Set up child's thread context to return through trap_return */
    extern void trap_return(void);
    child->thread.ra = (unsigned long)trap_return;
    /* Child's sp should point to its trap frame */
    child->thread.sp = (unsigned long)child_tf;

    enqueue(child);
    return child->pid;
}

int sys_exec(const char *path) {
    const unsigned char *data;
    int size;
    if (initrd_find_file(path, &data, &size) != 0)
        return -1;

    struct task_struct *current = get_current();

    /* Free old program if any */
    if (current->prog) free((void *)current->prog);
    if (current->user_stack) free((void *)current->user_stack);

    /* Allocate new program memory */
    void *prog = allocate((unsigned long)size);
    if (!prog) return -1;
    k_memcpy(prog, data, (unsigned long)size);
    asm volatile(".4byte 0x0000100F" ::: "memory");  /* fence.i */

    /* Allocate new user stack */
    void *ustack = allocate(USER_STACK_SIZE);
    if (!ustack) { free(prog); return -1; }

    current->prog = (unsigned long)prog;
    current->prog_size = (unsigned long)size;
    current->user_stack = (unsigned long)ustack;
    current->user_sp = (unsigned long)ustack + USER_STACK_SIZE;

    /* Set up trap frame for returning to user mode */
    struct trap_frame *tf = current->tf;
    tf->sepc = (unsigned long)prog;
    tf->sp = current->user_sp;

    return 0;
}

void sys_exit(int status) {
    (void)status;
    thread_exit();
}

long sys_waitpid(long pid) {
    /* Find the target task */
    struct task_struct *target = 0;
    struct task_struct *p = run_queue;
    struct task_struct *start = p;
    while (p) {
        if (p->pid == (int)pid) {
            target = p;
            break;
        }
        p = p->next;
        if (p == start) break;
    }

    if (!target) return -1;
    if (target->state == TASK_ZOMBIE) return pid;

    /* Block current task until target exits */
    struct task_struct *current = get_current();
    current->state = TASK_WAITING;
    current->wait_target = target;
    schedule();

    return pid;
}

int sys_stop(long pid) {
    struct task_struct *p = run_queue;
    if (!p) return -1;

    struct task_struct *start = p;
    while (p) {
        if (p->pid == (int)pid) {
            p->state = TASK_ZOMBIE;
            /* Wake up any waiters */
            struct task_struct *q = run_queue;
            struct task_struct *qs = q;
            while (q) {
                if (q->state == TASK_WAITING && q->wait_target == p) {
                    q->state = TASK_RUNNING;
                    q->wait_target = 0;
                }
                q = q->next;
                if (q == qs) break;
            }
            return 0;
        }
        p = p->next;
        if (p == start) break;
    }

    return -1;
}

/* ---- usleep: block current thread for a duration ---- */

static void usleep_wakeup(void *arg) {
    struct task_struct *task = (struct task_struct *)arg;
    task->pending_timer = 0;
    if (task->state == TASK_WAITING)
        task->state = TASK_RUNNING;
}

void sys_usleep(unsigned long usec) {
    struct task_struct *current = get_current();
    current->state = TASK_WAITING;
    current->pending_timer = 1;
    add_timer(usleep_wakeup, current, usec);
    schedule();
}

/* ---- Signal handling ---- */

unsigned long sys_signal(int signum, void (*handler)()) {
    if (signum < 0 || signum >= MAX_SIG) return 0;
    struct task_struct *current = get_current();
    unsigned long old = (unsigned long)current->sig_handlers[signum];
    current->sig_handlers[signum] = handler;
    return old;
}

int sys_kill(int pid, int signum) {
    if (signum < 0 || signum >= MAX_SIG) return -1;
    struct task_struct *p = run_queue;
    if (!p) return -1;
    struct task_struct *start = p;
    while (1) {
        if (p->pid == pid) {
            if (p->sig_handlers[signum]) {
                p->pending_sig = signum;
            } else {
                /* Default action: terminate */
                p->state = TASK_ZOMBIE;
                /* Wake up any waiters */
                struct task_struct *q = run_queue;
                struct task_struct *qs = q;
                while (1) {
                    if (q->state == TASK_WAITING && q->wait_target == p)
                        q->state = TASK_RUNNING;
                    q = q->next;
                    if (q == qs) break;
                }
            }
            return 0;
        }
        p = p->next;
        if (p == start) break;
    }
    return -1;
}

void sys_sigreturn(struct trap_frame *tf) {
    struct task_struct *current = get_current();
    uart_puts("[sigreturn] pid=");
    uart_putdec((unsigned long)current->pid);
    uart_puts("\r\n");

    /* Restore saved user context */
    if (current->saved_tf) {
        k_memcpy(tf, current->saved_tf, sizeof(struct trap_frame));
        free(current->saved_tf);
        current->saved_tf = 0;
    }
    /* Free signal stack */
    if (current->sig_stack) {
        free((void *)current->sig_stack);
        current->sig_stack = 0;
    }
}

/*
 * Check for pending signals before returning to user mode.
 * Called from do_trap after handling the trap.
 */
void check_pending_signal(struct trap_frame *tf) {
    struct task_struct *current = get_current();
    if (current->pending_sig < 0) return;

    int sig = current->pending_sig;
    current->pending_sig = -1;

    void (*handler)() = current->sig_handlers[sig];
    if (!handler) return;

    /* Save current user context */
    current->saved_tf = (struct trap_frame *)allocate(sizeof(struct trap_frame));
    if (!current->saved_tf) return;
    k_memcpy(current->saved_tf, tf, sizeof(struct trap_frame));

    /* Allocate signal handler stack */
    void *sig_stack = allocate(SIG_STACK_SIZE);
    if (!sig_stack) { free(current->saved_tf); current->saved_tf = 0; return; }
    current->sig_stack = (unsigned long)sig_stack;

    /* Build sigreturn trampoline on signal stack top */
    /* The trampoline: li a7, 11; ecall */
    unsigned long stack_top = (unsigned long)sig_stack + SIG_STACK_SIZE;
    unsigned int *trampoline = (unsigned int *)(stack_top - 8); // a riscv instruction is 4 bytes, we need 2 instructions for the trampoline
    trampoline[0] = 0x00b00893;  /* li a7, 11 */
    trampoline[1] = 0x00000073;  /* ecall */
    /* fence.i to flush I-cache */
    asm volatile(".4byte 0x0000100F" ::: "memory");

    /* Redirect execution to signal handler */
    tf->sepc = (unsigned long)handler;
    tf->sp = (unsigned long)trampoline;  /* stack below trampoline */
    tf->ra = (unsigned long)trampoline;  /* return to trampoline */
}
