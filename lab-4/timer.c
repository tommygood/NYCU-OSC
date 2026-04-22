/*
 * timer.c - Core timer using SBI Timer Extension + timer multiplexing.
 *
 * The SBI set_timer call programs the hardware timer to fire at a given
 * absolute time value. We maintain a sorted list of software timers and
 * reprogram the hardware to fire at the earliest expiration.
 */

#include "timer.h"

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

#define MAX_TIMERS 32

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
    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_pool[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
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
}

/* Reprogram hardware timer for the next earliest software timer, or 2s periodic */
static void reprogram_next(void) {
    if (timer_count > 0) {
        sbi_set_timer(timer_pool[timer_order[0]].expire);
    } else {
        /* Default: 2 second periodic timer */
        sbi_set_timer(rdtime() + 2 * timebase_freq);
    }
}

/* ---- Core timer interrupt handler ---- */

extern unsigned long saved_hart_id;

static void default_timer_print(void) {
    unsigned long secs = timer_get_seconds();
    uart_puts("boot time: ");
    uart_putdec(secs);

    uart_puts("\r\n");
}

void handle_timer_irq(void) {
    unsigned long now = rdtime();

    /* Fire all expired software timers */
    int fired = 0;
    while (timer_count > 0) {
        int idx = timer_order[0];
        if (timer_pool[idx].expire > now)
            break;

        /* Remove from sorted list */
        timer_pool[idx].active = 0;
        for (int i = 0; i < timer_count - 1; i++)
            timer_order[i] = timer_order[i + 1];
        timer_count--;

        /* Call the callback */
        timer_pool[idx].callback(timer_pool[idx].arg);
        fired = 1;
    }

    if (!fired) {
        /* No software timers fired - this is the periodic 2s timer */
        default_timer_print();
    }

    /* Reprogram for next event */
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

    /* Schedule first timer interrupt 2 seconds from now */
    sbi_set_timer(boot_time + 2 * timebase_freq);

    /* Enable timer interrupt: set STIE in sie (AFTER programming timer) */
    unsigned long stie = (1UL << 5);
    asm volatile("csrs sie, %0" :: "r"(stie));
}
