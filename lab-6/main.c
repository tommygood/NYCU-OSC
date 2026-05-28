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

/* ── Types ───────────────────────────────────────────────────────────────── */
typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long      size_t;

#include "mm.h"
#include "plic.h"
#include "timer.h"
#include "trap.h"
#include "thread.h"
#include "vm.h"

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
extern unsigned long dtb_get_timebase_freq(const void *fdt);
extern unsigned long dtb_get_plic_base(const void *fdt);
extern unsigned long dtb_get_uart_irq(const void *fdt);

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
 * initrd_find_file - find a file in the cpio archive and return a pointer
 * to its data and size.  Returns 0 on success, -1 on not found.
 * (global wrapper for thread.c to use)
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

int initrd_find_file(const char *filename,
                     const unsigned char **data_out, int *size_out) {
    if (!g_initrd) return -1;
    return initrd_find(g_initrd, filename, data_out, size_out);
}

/* ── SBI interface (sbi.c) ───────────────────────────────────────────────── */
extern long sbi_get_spec_version(void);
extern long sbi_get_impl_id(void);
extern long sbi_get_impl_version(void);
extern long sbi_probe_extension(long ext_id);
extern void sbi_timer_init(void);

/* ── Shell commands ──────────────────────────────────────────────────────── */

#ifdef QEMU
# define LOAD_ADDR (0x80200000UL + PAGE_OFFSET)
#else
# define LOAD_ADDR (0x00200000UL + PAGE_OFFSET)
#endif

#define NULL 0

#define BUF_SIZE 128

static void cmd_help(void) {
    uart_puts("Commands:\r\n"
              "  help          - this message\r\n"
              "  hello         - Hello World\r\n"
              "  info          - SBI/system info\r\n"
              "  ls            - list initrd files\r\n"
              "  cat <file>    - print initrd file\r\n"
              "  exec <file>   - load and run user program (threaded)\r\n"
              "  thread_test   - test thread scheduling\r\n"
              "  timer         - show timer seconds\r\n"
              "  setTimeout <s> <msg> - print msg after s seconds\r\n"
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

/* ── exec command: run user program as a thread ──────────────────────────── */

struct exec_arg {
    char filename[64];
};

static struct exec_arg *g_exec_arg;

static void exec_thread_fn(void) {
    struct exec_arg *ea = g_exec_arg;

    struct task_struct *cur = get_current();

    const uint8_t *data;
    int size;
    if (initrd_find_file(ea->filename, &data, &size) != 0) {
        uart_puts(ea->filename);
        uart_puts(": not found\r\n");
        free(ea);
        return;
    }
    /* Allocate and copy program */
    void *prog = allocate((unsigned long)size);
    if (!prog) { uart_puts("Failed to allocate prog\r\n"); free(ea); return; }
    for (int i = 0; i < size; i++) ((uint8_t *)prog)[i] = data[i];

    /* Allocate user stack */
    void *ustack = allocate(USER_STACK_SIZE);
    if (!ustack) { free(prog); free(ea); return; }

    cur->prog = (unsigned long)prog;
    cur->prog_size = (unsigned long)size;
    cur->user_stack = (unsigned long)ustack;
    cur->user_sp = USER_STACK_TOP;

    /* Create per-process page table */
    extern unsigned long *create_user_pgd(void);
    extern void map_pages(unsigned long *pgd, unsigned long va, unsigned long pa,
                          unsigned long size, unsigned long prot);
    extern void switch_mm(unsigned long *pgd);

    cur->pgd = create_user_pgd();
    if (!cur->pgd) { free(prog); free(ustack); free(ea); return; }

    /* Map user code at VA 0x0, stack below USER_STACK_TOP */
    map_pages(cur->pgd, USER_CODE_VA, VA_TO_PA((unsigned long)prog),
              (unsigned long)size, PROT_USER_RWX);
    map_pages(cur->pgd, USER_STACK_VA, VA_TO_PA((unsigned long)ustack),
              USER_STACK_SIZE, PROT_USER_RW);

    /* Switch to the new address space */
    switch_mm(cur->pgd);

    free(ea);

    /* Flush instruction cache after copying program code */
    asm volatile(".4byte 0x0000100F" ::: "memory");  /* fence.i */

    uart_puts("Running in U-mode (VA 0x0)\r\n");

    /* Build a trap frame on the kernel stack and sret to user mode */
    struct trap_frame tf;
    for (int i = 0; i < (int)(sizeof(tf) / sizeof(unsigned long)); i++)
        ((unsigned long *)&tf)[i] = 0;

    tf.sepc = USER_CODE_VA;        /* user program starts at VA 0x0 */
    tf.sp = USER_STACK_TOP;        /* user stack top */
    tf.tp = (unsigned long)cur;    /* preserve task pointer */
    tf.sstatus = SSTATUS_SPIE | SSTATUS_SUM;  /* SPP=0 (U-mode), SPIE=1, SUM=1 */

    /* Enter user mode via trap_return */
    extern void enter_user_mode(struct trap_frame *tf);
    enter_user_mode(&tf);
}

#define EXEC_FILENAME_MAX (BUF_SIZE - 5)  /* "exec " + filename fits in BUF_SIZE */
static void cmd_exec(const char *arg) {
    if (!arg || *arg == '\0') { uart_puts("Usage: exec <file>\r\n"); return; }
    if (!g_initrd) { uart_puts("No initrd\r\n"); return; }

    struct exec_arg *ea = (struct exec_arg *)allocate(sizeof(struct exec_arg));
    if (!ea) { uart_puts("Failed to allocate exec_arg\r\n"); return; }

    /* Copy filename */
    int i = 0;
    while (arg[i] && i < EXEC_FILENAME_MAX) { ea->filename[i] = arg[i]; i++; }
    ea->filename[i] = '\0';

    g_exec_arg = ea;
    struct task_struct *t = thread_create(exec_thread_fn);
    if (!t) { uart_puts("Failed to create thread\r\n"); free(ea); return; }

    /* Wait for the user program to finish */
    sys_waitpid((long)t->pid);
}

/* ── Thread test (Basic Exercise 1) ──────────────────────────────────────── */

static void foo(void) {
    for (int i = 0; i < 5; i++) {
        uart_puts("Thread id: ");
        uart_putdec((unsigned long)get_current()->pid);
        uart_puts(" ");
        uart_putdec((unsigned long)i);
        uart_puts("\r\n");
        for (int j = 0; j < 100000000; j++);
        schedule();
    }
}

static void cmd_thread_test(void) {
    for (int i = 0; i < 3; i++)
        thread_create(foo);
}

static void cmd_timer(void) {
    uart_puts("Seconds since boot: ");
    uart_putdec(timer_get_seconds());
    uart_puts("\r\n");
}

/* setTimeout */
#define MAX_TIMEOUTS 8

struct timeout_info {
    char message[64];
    unsigned long set_time;
    int active;
};

static struct timeout_info timeout_pool[MAX_TIMEOUTS];

static void timeout_cb(void *arg) {
    struct timeout_info *info = (struct timeout_info *)arg;
    uart_puts(info->message);
    info->active = 0;
}

static unsigned long parse_ulong(const char *s) {
    unsigned long r = 0;
    while (*s >= '0' && *s <= '9') {
        r = r * 10 + (*s - '0');
        s++;
    }
    return r;
}

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

    int idx = -1;
    for (int i = 0; i < MAX_TIMEOUTS; i++) {
        if (!timeout_pool[i].active) { idx = i; break; }
    }
    if (idx < 0) {
        uart_puts("setTimeout: pool full\r\n");
        return;
    }
    struct timeout_info *info = &timeout_pool[idx];
    info->active = 1;
    info->set_time = timer_get_seconds();
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

static void cmd_bootloader(void) {
    uart_puts("UART Bootloader ready. Waiting for kernel (BOOT protocol)...\r\n");
    asm volatile("csrci sstatus, 2");
    uart_init();

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

        /* Convert addresses back to physical for the new kernel */
        unsigned long load_pa = VA_TO_PA(LOAD_ADDR);
        unsigned long fdt_pa = VA_TO_PA((unsigned long)g_fdt);
        uart_puts("Done. Jumping to ");
        uart_hex(load_pa);
        uart_puts("\r\n");

        uart_init();
        /* jump_to_entry handles MMU disable and transition to physical */
        jump_to_entry(load_pa, saved_hart_id, fdt_pa);
        while (1) {}
    }
}

/* ── Shell ───────────────────────────────────────────────────────────────── */

static void dispatch(char *cmd, char *arg) {
    if      (k_strcmp(cmd, "help")  == 0) cmd_help();
    else if (k_strcmp(cmd, "hello") == 0) cmd_hello();
    else if (k_strcmp(cmd, "info")  == 0) cmd_info();
    else if (k_strcmp(cmd, "ls")    == 0) cmd_ls();
    else if (k_strcmp(cmd, "cat")         == 0) cmd_cat(arg);
    else if (k_strcmp(cmd, "exec")        == 0) cmd_exec(arg);
    else if (k_strcmp(cmd, "thread_test") == 0) cmd_thread_test();
    else if (k_strcmp(cmd, "timer")       == 0) cmd_timer();
    else if (k_strcmp(cmd, "setTimeout")  == 0) cmd_settimeout(arg);
    else if (k_strcmp(cmd, "bootloader")  == 0) cmd_bootloader();
    else if (k_strcmp(cmd, "ps")          == 0) list_processes();
    else if (cmd[0] != '\0') {
        uart_puts("Unknown: ");
        uart_puts(cmd);
        uart_puts("\r\n");
    }
}

static void shell_run(void) {
    char buf[BUF_SIZE];
    int pos;

    while (1) {
        uart_puts("\r\nkernel $ ");
        pos = 0;

        while (1) {
            char c = uart_getc();

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
    sbi_timer_init();

    plic_init();
    uart_flush_rx();
    uart_enable_irq();

    for (int irq; (irq = plic_claim()) != 0; )
        plic_complete(irq);

    timer_init();

    unsigned long seie = (1UL << 9);
    asm volatile("csrs sie, %0" :: "r"(seie));

    unsigned long sie_bit = (1UL << 1);
    asm volatile("csrs sstatus, %0" :: "r"(sie_bit));
}

/* ── Idle + shell thread ─────────────────────────────────────────────────── */

/* ── Kernel entry point ──────────────────────────────────────────────────── */
void kernel_main(void *fdt) {
    /* fdt is already a virtual address (start.S added PAGE_OFFSET) */
    g_fdt = fdt;

    /* DTB parser returns physical addresses — convert to virtual */
    unsigned long uart_base_pa = dtb_get_uart_base(fdt);
    unsigned long initrd_start_pa = dtb_get_initrd_start(fdt);
    unsigned long tb_freq      = dtb_get_timebase_freq(fdt);
    unsigned long plic_addr_pa = dtb_get_plic_base(fdt);
    unsigned long uart_irq_nr  = dtb_get_uart_irq(fdt);

    if (uart_base_pa) uart_set_base(PA_TO_VA(uart_base_pa));
    if (initrd_start_pa) g_initrd = (const void *)PA_TO_VA(initrd_start_pa);
    if (tb_freq)      timer_set_freq(tb_freq);
    if (plic_addr_pa) plic_set_base(PA_TO_VA(plic_addr_pa));
    if (uart_irq_nr)  plic_set_uart_irq(uart_irq_nr);

    uart_puts("\r\n\r\n");
    uart_puts("OSC2026 Lab 6 - Virtual Memory\r\n");
    uart_puts("DTB at "); uart_hex((unsigned long)fdt); uart_puts("\r\n");
    uart_puts("UART base (VA): "); uart_hex(PA_TO_VA(uart_base_pa)); uart_puts("\r\n");

    if (initrd_start_pa) {
        uart_puts("initrd at (VA): "); uart_hex(PA_TO_VA(initrd_start_pa)); uart_puts("\r\n");
    } else {
        uart_puts("No initrd found in DTB\r\n");
    }

    if (mm_init(fdt) == 0) uart_puts("MM ready.\r\n");
    else uart_puts("MM init failed.\r\n");

    /* Initialize framebuffer */
    extern void video_init(const void *fdt);
    video_init(fdt);

    /* Create idle thread (pid 0) and set tp */
    struct task_struct *idle_task = thread_create(idle);
    asm volatile("mv tp, %0" :: "r"(idle_task));

    /* Create shell thread */
    thread_create(shell_run);

    /* Set up interrupts */
    irq_init();
    uart_puts("Interrupts enabled.\r\n");

    uart_puts("Type 'help' for commands.\r\n");

    /* Start idle loop */
    idle();
}
