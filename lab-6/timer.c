/*
 * timer.c - Core timer using SBI Timer Extension + timer multiplexing.
 *
 * The SBI set_timer call programs the hardware timer to fire at a given
 * absolute time value. We maintain a sorted list of software timers and
 * reprogram the hardware to fire at the earliest expiration.
 */

#include "timer.h"
#include "trap.h"

extern void uart_puts(const char *s);
extern void uart_putdec(unsigned long n);
extern void uart_hex(unsigned long h);
extern void uart_putc(char c);

/* SBI Timer Extension (EID = 0x54494D45, FID = 0) */
extern void sbi_set_timer(unsigned long stime_value);

/* ---- Time reading ---- */

static inline unsigned long rdtime(void) {
    unsigned long t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

/* Timebase frequency (ticks per second), set during init */
static unsigned long timebase_freq = 10000000UL; /* default 10 MHz for QEMU */

void timer_set_freq(unsigned long freq) {
    timebase_freq = freq;
}

/* ---- Boot time tracking ---- */

static unsigned long boot_time = 0;

unsigned long timer_get_seconds(void) {
    return (rdtime() - boot_time) / timebase_freq;
}

/* ---- Timer multiplexing (sorted linked list) ---- */

#define MAX_TIMERS 128

struct timer_entry {
    unsigned long       expire;   /* absolute tick count */
    timer_callback_t    callback;
    void               *arg;
    int                 active;
};

static struct timer_entry timer_pool[MAX_TIMERS];
static int timer_order[MAX_TIMERS]; /* indices sorted by expire time */
static int timer_count = 0;

/* Insert a timer into the sorted order array */
static void timer_insert_sorted(int idx) {
    /* Find insertion point */
    int pos = timer_count;
    for (int i = 0; i < timer_count; i++) {
        if (timer_pool[idx].expire < timer_pool[timer_order[i]].expire) {
            pos = i;
            break;
        }
    }
    /* Shift right */
    for (int i = timer_count; i > pos; i--)
        timer_order[i] = timer_order[i - 1];
    timer_order[pos] = idx;
    timer_count++;
}

void add_timer(timer_callback_t callback, void *arg, unsigned long duration_us) {
    /* Save and disable interrupts to protect timer_order/timer_count */
    unsigned long sstatus;
    asm volatile("csrrc %0, sstatus, 2" : "=r"(sstatus));

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_pool[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* Restore SIE before printing */
        if (sstatus & 2) asm volatile("csrsi sstatus, 2");
        uart_puts("add_timer: no free slots\r\n");
        return;
    }

    unsigned long now = rdtime();
    unsigned long ticks = (duration_us * timebase_freq) / 1000000UL;

    timer_pool[idx].expire = now + ticks;
    timer_pool[idx].callback = callback;
    timer_pool[idx].arg = arg;
    timer_pool[idx].active = 1;

    timer_insert_sorted(idx);

    /* If this is now the earliest timer, reprogram hardware */
    if (timer_order[0] == idx) {
        sbi_set_timer(timer_pool[idx].expire);
    }

    /* Restore previous SIE state */
    if (sstatus & 2) asm volatile("csrsi sstatus, 2");
}

/* Reprogram hardware timer: use the earlier of next software timer or 1/32s */
static void reprogram_next(void) {
    unsigned long preempt = rdtime() + timebase_freq / 32;
    if (timer_count > 0 && timer_pool[timer_order[0]].expire < preempt) {
        sbi_set_timer(timer_pool[timer_order[0]].expire);
    } else {
        sbi_set_timer(preempt);
    }
}


/* ---- Core timer interrupt handler ---- */

extern unsigned long saved_hart_id;

static void default_timer_print(void *arg) {
    (void)arg;
    unsigned long secs = timer_get_seconds();
    //uart_puts("boot time: ");
    //uart_putdec(secs);
    //uart_puts("\r\n");
}

#define TIMER_TASK_PRIORITY 100

void handle_timer_irq(void) {
    unsigned long now = rdtime();

    /* Top half: check expired timers, enqueue callbacks as tasks */
    int fired = 0;
    while (timer_count > 0) {
        int idx = timer_order[0];
        if (timer_pool[idx].expire > now) // no timer expired yet
            break;

        /* Remove from sorted list */
        timer_pool[idx].active = 0;
        for (int i = 0; i < timer_count - 1; i++)
            timer_order[i] = timer_order[i + 1];
        timer_count--;

        /* Defer callback to bottom half via task queue */
        add_task(timer_pool[idx].callback, timer_pool[idx].arg, TIMER_TASK_PRIORITY);
        fired = 1;
    }

    if (!fired) {
        /* No software timers fired - defer periodic print to bottom half */
        // since only software timers will add the timer in timer pool, so if no software timer is fired, we can be sure that this interrupt is casued from default timer
        add_task(default_timer_print, 0, TIMER_TASK_PRIORITY);
    }

    /* Reprogram for next event */
    // set timer for next software timer if exists, otherwise set a default timer for 1/32 second later to ensure we can get timer interrupts for preemption
    reprogram_next();
}

/* ---- Initialization ---- */

void timer_init(void) {
    boot_time = rdtime();

    /* Clear timer pool */
    for (int i = 0; i < MAX_TIMERS; i++)
        timer_pool[i].active = 0;
    timer_count = 0;

    /* Clear any pending timer from OpenSBI before enabling STIE */
    sbi_set_timer((unsigned long)-1);

    /* Schedule first timer interrupt 1/32 second from now */
    sbi_set_timer(boot_time + timebase_freq / 32);

    /* Enable timer interrupt: set STIE in sie (AFTER programming timer) */
    unsigned long stie = (1UL << 5);
    asm volatile("csrs sie, %0" :: "r"(stie));
}
