/*
 * timer.h - Core timer and timer multiplexing.
 */
#ifndef TIMER_H
#define TIMER_H

typedef void (*timer_callback_t)(void *arg);

void timer_init(void);
void handle_timer_irq(void);

/* Timer multiplexing API */
void add_timer(timer_callback_t callback, void *arg, unsigned long duration_us);

/* Get elapsed seconds since boot */
unsigned long timer_get_seconds(void);

#endif /* TIMER_H */
