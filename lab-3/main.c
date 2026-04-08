/* ── External symbols ────────────────────────────────────────────────────── */
extern char          uart_getc(void);
extern void          uart_putc(char c);
extern void          uart_puts(const char *s);
extern void          uart_hex(unsigned long h);
extern void          uart_init(void);
extern void          uart_set_base(unsigned long base);
extern void          uart_putdec(unsigned long n);
extern unsigned long uart_read_u32_le(void);
extern void          jump_to_entry(unsigned long entry, unsigned long hart_id,
                                   unsigned long dtb_ptr);

/* ── Types ───────────────────────────────────────────────────────────────── */
typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long      size_t;

#include "mm.h"

/* ── Globals (saved by start.S after BSS is zeroed) ─────────────────────── */
unsigned long saved_hart_id;      /* written by start.S */

static const void *g_fdt    = 0;  /* DTB pointer passed by OpenSBI     */
static const void *g_initrd = 0;  /* initrd base from DTB /chosen node  */


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

/* Parse n ASCII hex characters into an integer. */
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

/*
 * initrd_ls – list all files: print "size name\r\n" for each entry.
 */
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

/*
 * initrd_cat – print the contents of a named file from the archive.
 */
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

/* ── SBI interface (sbi.c) ───────────────────────────────────────────────── */
extern long sbi_get_spec_version(void);
extern long sbi_get_impl_id(void);
extern long sbi_get_impl_version(void);

/* ── Shell commands ──────────────────────────────────────────────────────── */

/* Address where a received kernel binary is loaded and executed. */
#ifdef QEMU
# define LOAD_ADDR 0x80200000UL
#else
# define LOAD_ADDR 0x00200000UL
#endif

static void cmd_help(void) {
    uart_puts("Commands:\r\n"
              "  help          - this message\r\n"
              "  hello         - Hello World\r\n"
              "  info          - SBI/system info\r\n"
              "  ls            - list initrd files\r\n"
              "  cat <file>    - print initrd file\r\n"
              "  mmtest        - run allocator test\r\n"
              "  bootloader    - receive kernel via UART (BOOT protocol) and boot it\r\n");
}

static void cmd_hello(void) {
    uart_puts("Hello World!\r\n");
}

static void cmd_info(void) {
    /* Read current PC to confirm relocation: expect 0x84xxxxxx (QEMU RELOC_ADDR). */
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

static void cmd_mmtest(void) {
    uart_puts("Testing page allocation...\r\n");
    void *p1 = alloc(4000);
    void *p2 = alloc(8000);
    void *p3 = alloc(4000);
    void *p4 = alloc(4000);
    free(p1); free(p2); free(p3); free(p4);

    uart_puts("Testing dynamic allocator...\r\n");
    void *k1 = alloc(16), *k2 = alloc(32), *k3 = alloc(64), *k4 = alloc(128);
    free(k1); free(k2); free(k3); free(k4);
    void *k5 = alloc(16), *k6 = alloc(32);
    free(k5); free(k6);
    void *arr[100];
    for (int i = 0; i < 100; i++) arr[i] = alloc(128);
    for (int i = 0; i < 100; i++) free(arr[i]);

    void *mid = alloc(3072);
    if (mid) {
        uart_puts("3072-byte allocation success\r\n");
        free(mid);
    } else {
        uart_puts("3072-byte allocation failed\r\n");
    }

    void *bad = alloc(MAX_ALLOC_SIZE + 1);
    if (!bad) uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\r\n");
}

/*
 * cmd_bootloader – receive a kernel over UART (BOOT protocol) and boot it.
 *
 * BOOT protocol (little-endian):
 *   [4 bytes] magic  = 0x544F4F42  ("BOOT")
 *   [4 bytes] size   = byte count of the kernel binary
 *   [size bytes]     kernel binary
 *
 * The kernel was self-relocated to RELOC_ADDR by start.S, so LOAD_ADDR is
 * free to receive the new image without overwriting the running code.
 */
static void cmd_bootloader(void) {
    uart_puts("UART Bootloader ready. Waiting for kernel (BOOT protocol)...\r\n");

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
            char c = uart_getc();

            if (c == '\r' || c == '\n') {
                uart_puts("\r\n");
                buf[pos] = '\0';
                /* split "cmd arg" */
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

/* ── Kernel entry point ──────────────────────────────────────────────────── */
void kernel_main(void *fdt) {
    g_fdt = fdt;

    /* ── Parse DTB before any UART output ──────────────────────────────────
     * OpenSBI already initialized UART hardware, so no uart_init() needed.
     * Parse DTB first (pure memory reads, no UART required), then set the
     * correct base address before the first uart_puts call.
     */
    unsigned long uart_base    = dtb_get_uart_base(fdt);
    unsigned long initrd_start = dtb_get_initrd_start(fdt);

    if (uart_base)    uart_set_base(uart_base);
    if (initrd_start) g_initrd = (const void *)initrd_start;

    /* ── UART now has the correct base address ── */
    uart_puts("\r\n\r\n");
    uart_puts("OSC2026 Lab 3 - OrangePi RV2\r\n");
    uart_puts("DTB at "); uart_hex((unsigned long)fdt); uart_puts("\r\n");
    uart_puts("UART base: "); uart_hex(uart_base); uart_puts("\r\n");

    if (initrd_start) {
        uart_puts("initrd at "); uart_hex(initrd_start); uart_puts("\r\n");
    } else {
        uart_puts("No initrd found in DTB\r\n");
    }

    if (mm_init(fdt) == 0) uart_puts("MM ready. Type 'mmtest'.\r\n");
    else uart_puts("MM init failed.\r\n");

    uart_puts("Type 'help' for commands.\r\n");

    shell_run();
}
