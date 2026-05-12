/*
 * thread.h - Thread (task_struct) definitions and API.
 */
#ifndef THREAD_H
#define THREAD_H

#include "trap.h"

#define STACK_SIZE   0x4000   /* 16 KiB per kernel stack */
#define USER_STACK_SIZE 0x4000
#define SIG_STACK_SIZE 0x4000

/* Thread states */
#define TASK_RUNNING   0
#define TASK_ZOMBIE    1
#define TASK_WAITING   2

/* Signals */
#define MAX_SIG 32

struct thread_struct {
    unsigned long ra;
    unsigned long sp;
    unsigned long s[12];  /* s0-s11 */
};

struct task_struct {
    struct thread_struct thread;   /* must be first for switch_to offsets */
    int pid;
    int state;
    unsigned long kernel_sp;      /* top of kernel stack */
    unsigned long user_sp;        /* user stack top */
    unsigned long stack;          /* kernel stack base (for free) */
    unsigned long user_stack;     /* user stack base (for free) */
    unsigned long prog;           /* user program base (for free/fork) */
    unsigned long prog_size;      /* user program size */
    struct task_struct *next;
    struct task_struct *prev;
    struct task_struct *wait_target; /* task we are waiting on */
    void (*entry_fn)();              /* function to run on first schedule */
    int pending_timer;               /* has a pending usleep timer */
    /* Signal handling */
    void (*sig_handlers[MAX_SIG])(); /* registered signal handlers */
    int pending_sig;                 /* pending signal number, -1 if none. This is fine since we don't need to handle nested signal handler */
    struct trap_frame *saved_tf;     /* saved user context before signal handler */
    unsigned long sig_stack;         /* signal handler stack base (for free) */
};

/* Thread API */
struct task_struct *thread_create(void (*fn)());
void thread_exit(void);
void schedule(void);
void kill_zombies(void);
struct task_struct *get_current(void);
void idle(void);

/* Process API */
int sys_getpid(void);
long sys_fork(struct trap_frame *tf);
int sys_exec(const char *path, struct trap_frame *tf);
void sys_exit(int status);
long sys_waitpid(long pid);
int sys_stop(long pid);
unsigned long sys_signal(int signum, void (*handler)());
void sys_sigreturn(struct trap_frame *tf);
int sys_kill(int pid, int signum);
void check_pending_signal(struct trap_frame *tf);

/* Switch to (assembly) */
extern void switch_to(struct task_struct *prev, struct task_struct *next);

/* Global pid counter */
extern int nr_threads;

#endif /* THREAD_H */
