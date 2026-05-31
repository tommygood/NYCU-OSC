/*
 * thread.c - Thread creation, scheduling, and process management.
 */
#include "thread.h"
#include "mm.h"
#include "timer.h"
#include "vm.h"

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putdec(unsigned long n);
extern void *k_memset(void *s, int c, unsigned long n);

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
    task->pgd = 0;
    for (int i = 0; i < MAX_VMAS; i++) task->vmas[i].active = 0;
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

    /* Switch address space */
    if (next->pgd)
        switch_mm(next->pgd);
    else
        switch_mm(get_kernel_pgd());

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
            if (p->pgd) { free_user_pgd(p->pgd); p->pgd = 0; }
            if (p->stack) free((void *)p->stack);
            /* Don't free user_stack/prog here — free_user_pgd freed the physical pages */
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

/* ── List all processes ──────────────────────────────────────────────────── */

void list_processes(void) {
    if (!run_queue) { uart_puts("No processes\r\n"); return; }
    static const char *state_names[] = { "RUNNING", "ZOMBIE", "WAITING" };
    struct task_struct *p = run_queue;
    struct task_struct *start = p;
    uart_puts("PID  STATE    TIMER\r\n");
    while (1) {
        uart_puts(" ");
        uart_putdec((unsigned long)p->pid);
        uart_puts("   ");
        if (p->state >= 0 && p->state <= 2)
            uart_puts(state_names[p->state]);
        else {
            uart_puts("UNKNOWN(");
            uart_putdec((unsigned long)p->state);
            uart_puts(")");
        }
        uart_puts("  ");
        uart_putdec((unsigned long)p->pending_timer);
        uart_puts("\r\n");
        p = p->next;
        if (p == start) break;
    }
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

    /* Allocate user program and stack pages (separate physical pages) */
    void *child_prog = allocate(parent->prog_size);
    if (!child_prog) { free(kstack); free(child); return -1; }
    void *child_ustack = allocate(USER_STACK_SIZE);
    if (!child_ustack) { free(child_prog); free(kstack); free(child); return -1; }

    /* Copy parent's program and user stack to child's physical pages */
    k_memcpy(child_prog, (void *)parent->prog, parent->prog_size);
    k_memcpy(child_ustack, (void *)parent->user_stack, USER_STACK_SIZE);

    child->pid = nr_threads++;
    child->state = TASK_RUNNING;
    child->stack = (unsigned long)kstack;
    child->kernel_sp = (unsigned long)kstack + STACK_SIZE;
    child->user_stack = (unsigned long)child_ustack;
    child->prog = (unsigned long)child_prog;
    child->prog_size = parent->prog_size;
    child->user_sp = USER_STACK_TOP;
    child->wait_target = 0;
    child->pending_timer = 0;
    for (int i = 0; i < MAX_SIG; i++) child->sig_handlers[i] = parent->sig_handlers[i];
    child->pending_sig = -1;
    child->saved_tf = 0;
    child->sig_stack = 0;

    /* Create child's page table with kernel mappings */
    child->pgd = create_user_pgd();
    if (!child->pgd) {
        free(child_ustack); free(child_prog); free(kstack); free(child);
        return -1;
    }

    /* Map child's own physical pages at the same user VAs */
    map_pages(child->pgd, USER_CODE_VA, VA_TO_PA((unsigned long)child_prog),
              parent->prog_size, PROT_USER_RWX);
    map_pages(child->pgd, USER_STACK_VA, VA_TO_PA((unsigned long)child_ustack),
              USER_STACK_SIZE, PROT_USER_RW);

    /* Copy mmap regions from parent */
    for (int i = 0; i < MAX_VMAS; i++) {
        child->vmas[i] = parent->vmas[i];
        if (!parent->vmas[i].active) continue;
        /* Allocate new physical pages and copy content */
        unsigned long pte_prot = PTE_V | PTE_U | PTE_A | PTE_D;
        if (parent->vmas[i].prot & PROT_READ)  pte_prot |= PTE_R;
        if (parent->vmas[i].prot & PROT_WRITE) pte_prot |= PTE_W;
        if (parent->vmas[i].prot & PROT_EXEC)  pte_prot |= PTE_X;
        for (unsigned long off = 0; off < parent->vmas[i].size; off += PAGE_SIZE) {
            void *page = allocate(PAGE_SIZE);
            if (!page) continue;
            /* Copy content from parent's mapping via user VA */
            unsigned long src_va = parent->vmas[i].start + off;
            k_memcpy(page, (void *)src_va, PAGE_SIZE);
            map_pages(child->pgd, src_va, VA_TO_PA((unsigned long)page),
                      PAGE_SIZE, pte_prot);
        }
    }

    /* Copy parent's kernel stack */
    k_memcpy(kstack, (void *)parent->stack, STACK_SIZE);

    /* Calculate the trap frame offset in the new kernel stack */
    unsigned long tf_offset = (unsigned long)parent_tf - parent->stack;
    struct trap_frame *child_tf = (struct trap_frame *)((unsigned long)kstack + tf_offset);

    /* Child's fork returns 0, and set tp to child task */
    child_tf->a0 = 0;
    child_tf->tp = (unsigned long)child;
    child_tf->sepc += 4;  /* advance past ecall instruction */
    /* User sp stays the same VA — child has its own page table mapping */

    /* Set up child's thread context to return through trap_return */
    extern void trap_return(void);
    child->thread.ra = (unsigned long)trap_return;
    child->thread.sp = (unsigned long)child_tf;

    enqueue(child);
    return child->pid;
}

int sys_exec(const char *path, struct trap_frame *tf) {
    const unsigned char *data;
    int size;
    if (initrd_find_file(path, &data, &size) != 0)
        return -1;

    struct task_struct *current = get_current();

    /* Free old page table and mappings */
    if (current->pgd) free_user_pgd(current->pgd);

    /* Create new page table */
    current->pgd = create_user_pgd();
    if (!current->pgd) return -1;

    /* Allocate and copy program */
    void *prog = allocate((unsigned long)size);
    if (!prog) { free_user_pgd(current->pgd); current->pgd = 0; return -1; }
    k_memcpy(prog, data, (unsigned long)size);

    /* Allocate user stack */
    void *ustack = allocate(USER_STACK_SIZE);
    if (!ustack) { free(prog); free_user_pgd(current->pgd); current->pgd = 0; return -1; }

    current->prog = (unsigned long)prog;
    current->prog_size = (unsigned long)size;
    current->user_stack = (unsigned long)ustack;
    current->user_sp = USER_STACK_TOP;

    /* Map user code at VA 0x0 (read + execute) */
    map_pages(current->pgd, USER_CODE_VA, VA_TO_PA((unsigned long)prog),
              (unsigned long)size, PROT_USER_RWX);

    /* Map user stack */
    map_pages(current->pgd, USER_STACK_VA, VA_TO_PA((unsigned long)ustack),
              USER_STACK_SIZE, PROT_USER_RW);

    /* Register code and stack as VMAs so mmap detects overlaps */
    unsigned long prog_mapped_size = ((unsigned long)size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (int i = 0; i < MAX_VMAS; i++) current->vmas[i].active = 0;
    current->vmas[0].start = USER_CODE_VA;
    current->vmas[0].size = prog_mapped_size;
    current->vmas[0].prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    current->vmas[0].active = 1;
    current->vmas[1].start = USER_STACK_VA;
    current->vmas[1].size = USER_STACK_SIZE;
    current->vmas[1].prot = PROT_READ | PROT_WRITE;
    current->vmas[1].active = 1;

    /* Switch to new address space */
    switch_mm(current->pgd);
    asm volatile(".4byte 0x0000100F" ::: "memory");  /* fence.i */

    /* Set up trap frame for returning to user mode */
    tf->sepc = USER_CODE_VA;
    tf->sp = USER_STACK_TOP;

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

/* ---- mmap ---- */

static int vma_overlaps(struct vma *vmas, unsigned long start, unsigned long size) {
    unsigned long end = start + size;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!vmas[i].active) continue;
        unsigned long vs = vmas[i].start;
        unsigned long ve = vs + vmas[i].size;
        if (start < ve && end > vs) return 1;
    }
    return 0;
}

unsigned long sys_mmap(unsigned long addr, unsigned long length, int prot, int flags) {
    struct task_struct *current = get_current();
    if (!current->pgd) return (unsigned long)-1;

    /* Round length up to page size */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (length == 0) return (unsigned long)-1;

    /* Find the target address */
    unsigned long target;
    if (addr == 0) {
        /* Kernel chooses address — find a free region starting from MMAP_BASE */
        target = MMAP_BASE;
        while (vma_overlaps(current->vmas, target, length)) {
            target += PAGE_SIZE;
            if (target + length > USER_STACK_VA) return (unsigned long)-1;
        }
    } else {
        /* Check alignment and overlap */
        if ((addr & (PAGE_SIZE - 1)) || vma_overlaps(current->vmas, addr, length)) {
            /* Treat as hint — find a free region */
            target = MMAP_BASE;
            while (vma_overlaps(current->vmas, target, length)) {
                target += PAGE_SIZE;
                if (target + length > USER_STACK_VA) return (unsigned long)-1;
            }
        } else {
            target = addr; // use as exact
        }
    }

    /* Find a free VMA slot */
    int slot = -1;
    for (int i = 0; i < MAX_VMAS; i++) {
        if (!current->vmas[i].active) { slot = i; break; }
    }
    if (slot < 0) return (unsigned long)-1;

    /* Only allocate and map pages if accessible (not PROT_NONE) */
    if (prot != PROT_NONE) {
        /* Convert prot to PTE flags */
        unsigned long pte_prot = PTE_V | PTE_U | PTE_A | PTE_D;
        if (prot & PROT_READ)  pte_prot |= PTE_R;
        if (prot & PROT_WRITE) pte_prot |= PTE_W;
        if (prot & PROT_EXEC)  pte_prot |= PTE_X;

        /* Allocate and map pages */
        for (unsigned long off = 0; off < length; off += PAGE_SIZE) {
            void *page = allocate(PAGE_SIZE);
            if (!page) return (unsigned long)-1;
            k_memset(page, 0, PAGE_SIZE);
            map_pages(current->pgd, target + off, VA_TO_PA((unsigned long)page),
                      PAGE_SIZE, pte_prot);
        }

        /* Flush TLB so new mappings take effect */
        asm volatile("sfence.vma" ::: "memory");
    }

    /* Record the VMA */
    current->vmas[slot].start = target;
    current->vmas[slot].size = length;
    current->vmas[slot].prot = prot;
    current->vmas[slot].active = 1;

    return target;
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

    /* run the user signal handler in a new context on the signal handler stack */

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
