/* ── External symbols ────────────────────────────────────────────────────── */
extern char          uart_getc(void);
extern void          uart_putc(char c);
extern void          uart_puts(const char *s);
extern void          uart_hex(unsigned long h);
extern void          uart_init(void);
extern void          uart_set_base(unsigned long base);
extern void          uart_putdec(unsigned long n);
extern unsigned long uart_read_u32_le(void);
extern void          uart_enable_irq(void);
extern char          uart_async_getc(void);
extern void          uart_async_putc(char c);
extern void          uart_async_puts(const char *s);
extern int           uart_async_read_ready(void);
extern void          uart_flush_rx(void);
extern void          jump_to_entry(unsigned long entry, unsigned long hart_id,
                                   unsigned long dtb_ptr);
extern void          exec_save_and_switch(unsigned long entry, unsigned long user_sp);

/* ── Types ───────────────────────────────────────────────────────────────── */
typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long      size_t;

#include "mm.h"
#include "plic.h"
#include "timer.h"
#include "trap.h"

/* ── Globals ─────────────────────────────────────────────────────────────── */
unsigned long saved_hart_id;

static const void *g_fdt    = 0;
static const void *g_initrd = 0;

/* ── String helpers (string.c) ───────────────────────────────────────────── */
extern size_t k_strlen(const char *s);
extern int    k_strcmp(const char *a, const char *b);
extern int    k_strncmp(const char *a, const char *b, size_t n);

/* ── DTB helpers (dtb.c) ─────────────────────────────────────────────────── */
extern unsigned long dtb_get_uart_base(const void *fdt);
extern unsigned long dtb_get_initrd_start(const void *fdt);

/* ── cpio (newc) parser ──────────────────────────────────────────────────── */
struct cpio_t {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

static unsigned int hextoi(const char *s, int n) {
    unsigned int r = 0;
    while (n-- > 0) {
        r <<= 4;
        if (*s >= 'A')
            r += *s++ - 'A' + 10;
        else
            r += *s++ - '0';
    }
    return r;
}

static int align4(int n) { return (n + 3) & ~3; }

static void initrd_ls(const void *rd) {
    const uint8_t *p = (const uint8_t *)rd;
    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;
        if (k_strncmp(hdr->magic, "070701", 6) != 0) {
            uart_puts("Invalid cpio magic\r\n");
            return;
        }
        int namesize = (int)hextoi(hdr->namesize, 8);
        int filesize = (int)hextoi(hdr->filesize, 8);
        const char *name = (const char *)p + sizeof(struct cpio_t);
        if (k_strcmp(name, "TRAILER!!!") == 0) return;
        uart_putdec((unsigned long)filesize);
        uart_putc(' ');
        uart_puts(name);
        uart_puts("\r\n");
        p += align4((int)sizeof(struct cpio_t) + namesize)
           + align4(filesize);
    }
}

static void initrd_cat(const void *rd, const char *filename) {
    const uint8_t *p = (const uint8_t *)rd;
    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;
        if (k_strncmp(hdr->magic, "070701", 6) != 0) {
            uart_puts("Invalid cpio magic\r\n");
            return;
        }
        int namesize = (int)hextoi(hdr->namesize, 8);
        int filesize = (int)hextoi(hdr->filesize, 8);
        const char *name = (const char *)p + sizeof(struct cpio_t);
        if (k_strcmp(name, "TRAILER!!!") == 0) {
            uart_puts(filename);
            uart_puts(": No such file\r\n");
            return;
        }
        if (k_strcmp(name, filename) == 0) {
            const char *data =
                (const char *)p +
                align4((int)sizeof(struct cpio_t) + namesize);
            for (int i = 0; i < filesize; i++) {
                if (data[i] == '\n') uart_putc('\r');
                uart_putc(data[i]);
            }
            return;
        }
        p += align4((int)sizeof(struct cpio_t) + namesize)
           + align4(filesize);
    }
}

/*
 * initrd_find - find a file in the cpio archive and return a pointer
 * to its data and size.  Returns 0 on success, -1 on not found.
 */
static int initrd_find(const void *rd, const char *filename,
                       const uint8_t **data_out, int *size_out) {
    const uint8_t *p = (const uint8_t *)rd;
    while (1) {
        const struct cpio_t *hdr = (const struct cpio_t *)p;
        if (k_strncmp(hdr->magic, "070701", 6) != 0) return -1;
        int namesize = (int)hextoi(hdr->namesize, 8);
        int filesize = (int)hextoi(hdr->filesize, 8);
        const char *name = (const char *)p + sizeof(struct cpio_t);
        if (k_strcmp(name, "TRAILER!!!") == 0) return -1;
        if (k_strcmp(name, filename) == 0) {
            *data_out = p + align4((int)sizeof(struct cpio_t) + namesize);
            *size_out = filesize;
            return 0;
        }
        p += align4((int)sizeof(struct cpio_t) + namesize)
           + align4(filesize);
    }
}

/* ── SBI interface (sbi.c) ───────────────────────────────────────────────── */
extern long sbi_get_spec_version(void);
extern long sbi_get_impl_id(void);
extern long sbi_get_impl_version(void);
extern long sbi_probe_extension(long ext_id);
extern void sbi_timer_init(void);

/* ── Shell commands ──────────────────────────────────────────────────────── */

#ifdef QEMU
# define LOAD_ADDR 0x80200000UL
#else
# define LOAD_ADDR 0x00200000UL
#endif

/* User program stack size */
#define USER_STACK_SIZE  PAGE_SIZE

static void cmd_help(void) {
    uart_puts("Commands:\r\n"
              "  help          - this message\r\n"
              "  hello         - Hello World\r\n"
              "  info          - SBI/system info\r\n"
              "  ls            - list initrd files\r\n"
              "  cat <file>    - print initrd file\r\n"
              "  exec <file>   - load and run user program in U-mode\r\n"
              "  timer         - show timer seconds\r\n"
              "  setTimeout <s> <msg> - print msg after s seconds\r\n"
              "  mmtest        - run allocator test\r\n"
              "  bootloader    - receive kernel via UART (BOOT protocol)\r\n");
}

static void cmd_hello(void) {
    uart_puts("Hello World!\r\n");
}

static void cmd_info(void) {
    unsigned long pc;
    asm volatile("auipc %0, 0" : "=r"(pc));
    uart_puts("Running at:    "); uart_hex(pc);                                    uart_puts("\r\n");
    uart_puts("SBI spec ver:  "); uart_hex((unsigned long)sbi_get_spec_version()); uart_puts("\r\n");
    uart_puts("SBI impl ID:   "); uart_hex((unsigned long)sbi_get_impl_id());      uart_puts("\r\n");
    uart_puts("SBI impl ver:  "); uart_hex((unsigned long)sbi_get_impl_version()); uart_puts("\r\n");
    uart_puts("DTB at:        "); uart_hex((unsigned long)g_fdt);                  uart_puts("\r\n");
    if (g_initrd) {
        uart_puts("initrd at:     "); uart_hex((unsigned long)g_initrd);           uart_puts("\r\n");
    } else {
        uart_puts("initrd:        not found\r\n");
    }
}

static void cmd_ls(void) {
    if (!g_initrd) { uart_puts("No initrd\r\n"); return; }
    initrd_ls(g_initrd);
}

static void cmd_cat(const char *arg) {
    if (!arg || *arg == '\0') { uart_puts("Usage: cat <file>\r\n"); return; }
    if (!g_initrd) { uart_puts("No initrd\r\n"); return; }
    initrd_cat(g_initrd, arg);
}

/*
 * cmd_exec - Load a user program from initramfs and execute it in U-mode.
 */
static void cmd_exec(const char *arg) {
    if (!arg || *arg == '\0') { uart_puts("Usage: exec <file>\r\n"); return; }
    if (!g_initrd) { uart_puts("No initrd\r\n"); return; }

    const uint8_t *data;
    int size;
    if (initrd_find(g_initrd, arg, &data, &size) != 0) {
        uart_puts(arg);
        uart_puts(": not found\r\n");
        return;
    }

    /* Allocate memory for user program */
    size_t alloc_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (alloc_size < PAGE_SIZE) alloc_size = PAGE_SIZE;
    uint8_t *prog = (uint8_t *)allocate(alloc_size);
    if (!prog) {
        uart_puts("Failed to allocate memory for program\r\n");
        return;
    }

    /* Copy program */
    for (int i = 0; i < size; i++)
        prog[i] = data[i];

    /* Allocate user stack */
    uint8_t *user_stack = (uint8_t *)allocate(USER_STACK_SIZE);
    if (!user_stack) {
        uart_puts("Failed to allocate user stack\r\n");
        free(prog);
        return;
    }

    uart_puts("Running '");
    uart_puts(arg);
    uart_puts("' at ");
    uart_hex((unsigned long)prog);
    uart_puts(" in U-mode\r\n");

    /* User stack grows downward: pass top of allocated area */
    unsigned long user_sp = (unsigned long)user_stack + USER_STACK_SIZE;

    /* Save kernel context and switch to U-mode.
     * exec_save_and_switch never returns directly; when the user
     * program calls exit (syscall 0), exec_return_to_kernel()
     * restores context and returns here. */
    exec_save_and_switch((unsigned long)prog, user_sp);

    uart_puts("User program exited, back in S-mode\r\n");

    free(user_stack);
    free(prog);
}

static void cmd_timer(void) {
    uart_puts("Seconds since boot: ");
    uart_putdec(timer_get_seconds());
    uart_puts("\r\n");
}

/* setTimeout: stores the message and set-time, prints them when timer fires */
struct timeout_info {
    char message[64];
    unsigned long set_time;
};

static struct timeout_info timeout_pool[8];
static int timeout_pool_idx = 0;

static void timeout_cb(void *arg) {
    struct timeout_info *info = (struct timeout_info *)arg;
    //uart_puts("[setTimeout] \"");
    uart_puts(info->message);
    /*
    uart_puts("\" at ");
    uart_putdec(timer_get_seconds());
    uart_puts("s (set at ");
    uart_putdec(info->set_time);
    uart_puts("s)\r\n");
    */
}

static unsigned long parse_ulong(const char *s) {
    unsigned long r = 0;
    while (*s >= '0' && *s <= '9') {
        r = r * 10 + (*s - '0');
        s++;
    }
    return r;
}

/* Skip digits and whitespace to get to the message part */
static const char *skip_to_message(const char *s) {
    while (*s >= '0' && *s <= '9') s++;
    while (*s == ' ') s++;
    return s;
}

static void cmd_settimeout(const char *arg) {
    if (!arg || *arg == '\0') {
        uart_puts("Usage: setTimeout <seconds> <message>\r\n");
        return;
    }
    unsigned long secs = parse_ulong(arg);
    const char *msg = skip_to_message(arg);

    /* Store info in pool */
    struct timeout_info *info = &timeout_pool[timeout_pool_idx % 8];
    timeout_pool_idx++;
    info->set_time = timer_get_seconds();
    /* Copy message */
    int i = 0;
    while (msg[i] && i < 63) { info->message[i] = msg[i]; i++; }
    info->message[i] = '\0';

    add_timer(timeout_cb, info, secs * 1000000UL);
    uart_puts("setTimeout: \"");
    uart_puts(info->message);
    uart_puts("\" after ");
    uart_putdec(secs);
    uart_puts("s\r\n");
}

static void cmd_mmtest(void) {
  dump_free_lists();
  uart_puts("\n===== Part 1 =====\n");

  void *p1 = allocate(129);
  free(p1);

  uart_puts("\n=== Part 1 End ===\n");

  uart_puts("\n===== Part 2 =====\n");

  int NUM_BLOCKS_AT_ORDER_0 = 0;
  int NUM_BLOCKS_AT_ORDER_1 = 0;
  int NUM_BLOCKS_AT_ORDER_2 = 0;
  int NUM_BLOCKS_AT_ORDER_3 = 0;

  void *ps0[NUM_BLOCKS_AT_ORDER_0];
  void *ps1[NUM_BLOCKS_AT_ORDER_1];
  void *ps2[NUM_BLOCKS_AT_ORDER_2];
  void *ps3[NUM_BLOCKS_AT_ORDER_3];
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i)
      ps0[i] = allocate(4096);
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i)
      ps1[i] = allocate(8192);
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i)
      ps2[i] = allocate(16384);
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i)
      ps3[i] = allocate(32768);

  uart_puts("\n-----------\n");

  long MAX_BLOCK_SIZE = PAGE_SIZE * (1 << MAX_ORDER);

  void *c1, *c2, *c3, *c4, *c5, *c6, *c7, *c8, *p2, *p3, *p4, *p5, *p6, *p7;

  p1 = allocate(4095);
  free(p1);
  p1 = allocate(4095);

  c1 = allocate(1000);
  c2 = allocate(1023);
  c3 = allocate(999);
  c4 = allocate(1010);
  free(c3);
  c5 = allocate(989);
  c3 = allocate(88);
  c6 = allocate(1001);
  free(c3);
  c7 = allocate(2045);
  c8 = allocate(1);

  p2 = allocate(4096);
  free(c8);
  p3 = allocate(16000);
  free(p1);
  free(c7);
  p4 = allocate(4097);
  p5 = allocate(MAX_BLOCK_SIZE + 1);
  p6 = allocate(MAX_BLOCK_SIZE);
  free(p2);
  free(p4);
  p7 = allocate(7197);

  free(p6);
  free(p3);
  free(p7);
  free(c1);
  free(c6);
  free(c2);
  free(c5);
  free(c4);

  uart_puts("\n-----------\n");

  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i)
      free(ps0[i]);
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i)
      free(ps1[i]);
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i)
      free(ps2[i]);
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i)
      free(ps3[i]);

  uart_puts("\n=== Part 2 End ===\n");
}

static void cmd_bootloader(void) {
    uart_puts("UART Bootloader ready. Waiting for kernel (BOOT protocol)...\r\n");

    /* Disable UART interrupts so uart_getc() (polling) works correctly.
     * The IRQ handler would steal bytes from the FIFO before getc sees them. */
    /* Disable all interrupts — the IRQ handler would steal bytes from
     * the FIFO before polling uart_getc() sees them, and stale timer
     * interrupts can fire into the new kernel before it sets up stvec. */
    asm volatile("csrci sstatus, 2");   /* clear SIE — disable all S-mode interrupts */
    uart_init();   /* resets IER to 0 — disables all UART interrupts */

    while (1) {
        unsigned long magic = uart_read_u32_le();
        if (magic != 0x544F4F42UL) {
            uart_puts("Bad magic, retrying...\r\n");
            continue;
        }

        unsigned long size = uart_read_u32_le();
        uart_puts("Receiving ");
        uart_putdec(size);
        uart_puts(" bytes to ");
        uart_hex(LOAD_ADDR);
        uart_puts("...\r\n");

        uint8_t *dst = (uint8_t *)LOAD_ADDR;
        for (unsigned long i = 0; i < size; i++)
            dst[i] = (uint8_t)uart_getc();

        uart_puts("Done. Jumping to ");
        uart_hex(LOAD_ADDR);
        uart_puts("\r\n");

        uart_init();
        jump_to_entry(LOAD_ADDR, saved_hart_id, (unsigned long)g_fdt);
        while (1) {}
    }
}

/* ── Shell ───────────────────────────────────────────────────────────────── */

static void dispatch(char *cmd, char *arg) {
    if      (k_strcmp(cmd, "help")  == 0) cmd_help();
    else if (k_strcmp(cmd, "hello") == 0) cmd_hello();
    else if (k_strcmp(cmd, "info")  == 0) cmd_info();
    else if (k_strcmp(cmd, "ls")    == 0) cmd_ls();
    else if (k_strcmp(cmd, "cat")        == 0) cmd_cat(arg);
    else if (k_strcmp(cmd, "exec")       == 0) cmd_exec(arg);
    else if (k_strcmp(cmd, "timer")      == 0) cmd_timer();
    else if (k_strcmp(cmd, "setTimeout")  == 0) cmd_settimeout(arg);
    else if (k_strcmp(cmd, "mmtest")     == 0) cmd_mmtest();
    else if (k_strcmp(cmd, "bootloader") == 0) cmd_bootloader();
    else if (cmd[0] != '\0') {
        uart_puts("Unknown: ");
        uart_puts(cmd);
        uart_puts("\r\n");
    }
}

static void shell_run(void) {
    char buf[128];
    int pos;

    while (1) {
        uart_puts("\r\n$ ");

        pos = 0;

        while (1) {
            /* Use async (interrupt-driven) UART read */
            char c = uart_async_getc();

            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                buf[pos] = '\0';
                char *cmd = buf;
                char *arg = 0;
                for (int i = 0; buf[i]; i++) {
                    if (buf[i] == ' ') {
                        buf[i] = '\0';
                        arg = buf + i + 1;
                        break;
                    }
                }
                dispatch(cmd, arg);
                break;
            } else if (c == 0x7f || c == '\b') {
                if (pos > 0) {
                    pos--;
                    uart_putc('\b');
                    uart_putc(' ');
                    uart_putc('\b');
                }
            } else if (pos < (int)(sizeof(buf) - 1)) {
                buf[pos++] = c;
                uart_putc(c);
            }
        }
    }
}

/* ── Interrupt initialization ────────────────────────────────────────────── */

static void irq_init(void) {
    /* 0. Detect SBI timer support (new vs legacy) */
    sbi_timer_init();

    /* 1. Configure devices BEFORE enabling CPU interrupts */
    plic_init();
    uart_flush_rx();       /* drain stale UART data */
    uart_enable_irq();

    /* Drain any pending PLIC interrupts left by OpenSBI */
    for (int irq; (irq = plic_claim()) != 0; )
        plic_complete(irq);

    /* 2. Initialize timer (sets STIE in sie and programs first event) */
    timer_init();

    /* 3. Enable external interrupts: set SEIE (bit 9) in sie */
    unsigned long seie = (1UL << 9);
    asm volatile("csrs sie, %0" :: "r"(seie));

    /* 4. Enable global supervisor interrupts LAST */
    unsigned long sie_bit = (1UL << 1);
    asm volatile("csrs sstatus, %0" :: "r"(sie_bit));

    /* empty */
}

/* ── Kernel entry point ──────────────────────────────────────────────────── */
void kernel_main(void *fdt) {
    g_fdt = fdt;

    unsigned long uart_base    = dtb_get_uart_base(fdt);
    unsigned long initrd_start = dtb_get_initrd_start(fdt);

    if (uart_base)    uart_set_base(uart_base);
    if (initrd_start) g_initrd = (const void *)initrd_start;

    uart_puts("\r\n\r\n");
    uart_puts("OSC2026 Lab 4 - Exception and Interrupt\r\n");
    uart_puts("DTB at "); uart_hex((unsigned long)fdt); uart_puts("\r\n");
    uart_puts("UART base: "); uart_hex(uart_base); uart_puts("\r\n");

    if (initrd_start) {
        uart_puts("initrd at "); uart_hex(initrd_start); uart_puts("\r\n");
    } else {
        uart_puts("No initrd found in DTB\r\n");
    }

    if (mm_init(fdt) == 0) uart_puts("MM ready.\r\n");
    else uart_puts("MM init failed.\r\n");

    /* Set up interrupts (PLIC, UART, timer) */
    irq_init();
    uart_puts("Interrupts enabled (timer, UART, external).\r\n");

    uart_puts("Type 'help' for commands.\r\n");

    shell_run();
}
