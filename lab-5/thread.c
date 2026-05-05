/*
 * thread.c - Thread creation, scheduling, and process management.
 */
#include "thread.h"
#include "mm.h"

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
        if (p->state == TASK_ZOMBIE) {
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
    /* Should never reach here */
    while (1);
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

/* Helper: memcpy */
static void *k_memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

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
    extern void child_ret_from_fork(void);
    child->thread.ra = (unsigned long)child_ret_from_fork;
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

    /* Clear SPP (return to U-mode), set SPIE (enable interrupts on sret) */
    tf->sstatus &= ~SSTATUS_SPP;
    tf->sstatus |= SSTATUS_SPIE;

    /* Clear registers */
    tf->a0 = 0;

    return 0;
}

void sys_exit(int status) {
    (void)status;
    thread_exit();
}

long sys_waitpid(long pid) {
    struct task_struct *current = get_current();

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
