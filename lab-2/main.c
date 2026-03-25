/*
 * Lab 2: Bootloader, Devicetree, Initial Ramdisk
 *
 * Basic Exercise 1 – UART bootloader: 'load' command
 * Basic Exercise 2 – Devicetree: read UART base from /soc/serial
 * Basic Exercise 3 – initrd: 'ls' and 'cat' commands over cpio archive
 */

/* ── External symbols ────────────────────────────────────────────────────── */
extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_init(void);
extern void uart_set_base(unsigned long base);
extern void uart_flush_rx(void);
extern void jump_to_entry(unsigned long entry, unsigned long hart_id,
                          unsigned long dtb_ptr);

/* ── Types ───────────────────────────────────────────────────────────────── */
typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      size_t;
typedef unsigned long      uintptr_t;

/* ── Globals (saved by start.S after BSS is zeroed) ─────────────────────── */
unsigned long saved_hart_id;      /* written by start.S */

static const void *g_fdt    = 0;  /* DTB pointer passed by OpenSBI     */
static const void *g_initrd = 0;  /* initrd base from DTB /chosen node  */

/* ── UART helpers ────────────────────────────────────────────────────────── */
static void uart_putdec(unsigned long n) {
    char buf[20];
    int i = 0;
    if (n == 0) { uart_putc('0'); return; }
    while (n) { buf[i++] = '0' + (int)(n % 10); n /= 10; }
    while (i--) uart_putc(buf[i]);
}

/* Read one little-endian 32-bit word from UART (for bootloader protocol). */
static unsigned long uart_read_u32_le(void) {
    unsigned long r = 0;
    r |= (unsigned long)(unsigned char)uart_getc() <<  0;
    r |= (unsigned long)(unsigned char)uart_getc() <<  8;
    r |= (unsigned long)(unsigned char)uart_getc() << 16;
    r |= (unsigned long)(unsigned char)uart_getc() << 24;
    return r;
}

/* ── String helpers ──────────────────────────────────────────────────────── */
static size_t k_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int k_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int k_strncmp(const char *a, const char *b, size_t n) {
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

/* ── FDT (flattened devicetree) parser ───────────────────────────────────── */
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009
#define FDT_MAGIC       0xd00dfeed

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static uint32_t bswap32(uint32_t x) {
    return ((x & 0xff000000u) >> 24) | ((x & 0x00ff0000u) >>  8)
         | ((x & 0x0000ff00u) <<  8) | ((x & 0x000000ffu) << 24);
}

static uint64_t bswap64(uint64_t x) {
    return ((uint64_t)bswap32((uint32_t)(x & 0xffffffffu)) << 32)
         | bswap32((uint32_t)(x >> 32));
}

static const void *align_up_ptr(const void *ptr, size_t align) {
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

/* Match a DTB node name (possibly "foo@addr") against a plain name "foo". */
static int node_name_match(const char *node_name, const char *comp) {
    if (k_strcmp(node_name, comp) == 0) return 1;
    size_t len = k_strlen(comp);
    return k_strncmp(node_name, comp, len) == 0 && node_name[len] == '@';
}

/*
 * fdt_path_offset – locate a node by path, returns byte offset from fdt base.
 * Returns -1 on error.
 */
static int fdt_path_offset(const void *fdt, const char *path) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (bswap32(hdr->magic) != FDT_MAGIC)
        return -1;

    /* split path into components (work on a stack buffer, not a heap) */
    char buf[256];
    size_t plen = k_strlen(path);
    if (plen >= sizeof(buf)) return -1;
    for (size_t i = 0; i <= plen; i++) buf[i] = path[i];

    char *comps[32];
    int ncomp = 0;
    char *start = (buf[0] == '/') ? buf + 1 : buf;
    char *p = start;
    /* tokenise by '/' */
    while (*p && ncomp < 32) {
        comps[ncomp++] = p;
        while (*p && *p != '/') p++;
        if (*p == '/') *p++ = '\0';
    }

    const uint8_t *cur =
        (const uint8_t *)fdt + bswap32(hdr->off_dt_struct);
    int depth = 0, matched = 0;

    while (1) {
        const uint8_t *tp = cur;
        uint32_t token = bswap32(*(const uint32_t *)cur);
        cur += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)cur;
            cur = (const uint8_t *)align_up_ptr(
                      cur + k_strlen(name) + 1, 4);
            if (depth != 0 && depth == matched + 1 &&
                node_name_match(name, comps[matched])) {
                matched++;
                if (matched == ncomp)
                    return (int)(tp - (const uint8_t *)fdt);
            }
            depth++;
        } else if (token == FDT_END_NODE) {
            depth--;
            if (matched > depth) matched = depth;
        } else if (token == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t *)cur); cur += 4;
            cur += 4;  /* skip nameoff */
            cur = (const uint8_t *)align_up_ptr(cur + len, 4);
        } else if (token == FDT_NOP) {
            /* skip */
        } else { /* FDT_END */
            return -1;
        }
    }
}

/*
 * fdt_getprop – retrieve a property value from the node at nodeoffset.
 * Returns pointer to raw big-endian data; sets *lenp to byte length.
 */
static const void *fdt_getprop(const void *fdt, int nodeoffset,
                                const char *name, int *lenp) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    uint32_t strs_off = bswap32(hdr->off_dt_strings);

    const uint8_t *cur = (const uint8_t *)fdt + nodeoffset;
    if (bswap32(*(const uint32_t *)cur) != FDT_BEGIN_NODE) return 0;
    cur += 4;
    cur = (const uint8_t *)align_up_ptr(
              cur + k_strlen((const char *)cur) + 1, 4);

    int depth = 0;
    while (1) {
        uint32_t tok = bswap32(*(const uint32_t *)cur);
        cur += 4;

        if (tok == FDT_BEGIN_NODE) {
            cur = (const uint8_t *)align_up_ptr(
                      cur + k_strlen((const char *)cur) + 1, 4);
            depth++;
        } else if (tok == FDT_END_NODE) {
            if (depth == 0) return 0;
            depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len     = bswap32(*(const uint32_t *)cur); cur += 4;
            uint32_t nameoff = bswap32(*(const uint32_t *)cur); cur += 4;
            const void *val  = cur;
            cur = (const uint8_t *)align_up_ptr(cur + len, 4);
            if (depth == 0) {
                const char *pname =
                    (const char *)fdt + strs_off + nameoff;
                if (k_strcmp(pname, name) == 0) {
                    if (lenp) *lenp = (int)len;
                    return val;
                }
            }
        } else if (tok == FDT_NOP) {
            /* skip */
        } else { /* FDT_END */
            return 0;
        }
    }
}

/* ── DTB helpers ─────────────────────────────────────────────────────────── */

/*
 * Read UART base address from /soc/serial reg property.
 * The reg property is: <addr_hi addr_lo size_hi size_lo> (big-endian u32s).
 */
static unsigned long dtb_get_uart_base(const void *fdt) {
    int off = fdt_path_offset(fdt, "/soc/serial");
    if (off < 0) return 0;
    int len;
    const uint32_t *reg =
        (const uint32_t *)fdt_getprop(fdt, off, "reg", &len);
    if (!reg || len < 8) return 0;
    unsigned long hi = bswap32(reg[0]);
    unsigned long lo = bswap32(reg[1]);
    return (hi << 32) | lo;
}

/*
 * Read initrd start address from /chosen linux,initrd-start.
 * May be 4 bytes (u32) or 8 bytes (u64), both big-endian.
 */
static unsigned long dtb_get_initrd_start(const void *fdt) {
    int off = fdt_path_offset(fdt, "/chosen");
    if (off < 0) return 0;
    int len;
    const void *prop =
        fdt_getprop(fdt, off, "linux,initrd-start", &len);
    if (!prop) return 0;
    if (len == 4) return bswap32(*(const uint32_t *)prop);
    if (len == 8) return bswap64(*(const uint64_t *)prop);
    return 0;
}

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

/* ── SBI interface ───────────────────────────────────────────────────────── */
struct sbiret { long error; long value; };

static struct sbiret sbi_ecall(int ext, int fid,
                               unsigned long a0, unsigned long a1,
                               unsigned long a2, unsigned long a3,
                               unsigned long a4, unsigned long a5) {
    struct sbiret ret;
    register unsigned long _a0 asm("a0") = a0;
    register unsigned long _a1 asm("a1") = a1;
    register unsigned long _a2 asm("a2") = a2;
    register unsigned long _a3 asm("a3") = a3;
    register unsigned long _a4 asm("a4") = a4;
    register unsigned long _a5 asm("a5") = a5;
    register unsigned long  a6 asm("a6") = (unsigned long)fid;
    register unsigned long  a7 asm("a7") = (unsigned long)ext;
    asm volatile("ecall"
                 : "+r"(_a0), "+r"(_a1)
                 : "r"(_a2), "r"(_a3), "r"(_a4), "r"(_a5),
                   "r"(a6), "r"(a7)
                 : "memory");
    ret.error = _a0;
    ret.value = _a1;
    return ret;
}

#define SBI_EXT_BASE  0x10
static long sbi_get_spec_version(void) {
    return sbi_ecall(SBI_EXT_BASE, 0, 0,0,0,0,0,0).value;
}
static long sbi_get_impl_id(void) {
    return sbi_ecall(SBI_EXT_BASE, 1, 0,0,0,0,0,0).value;
}
static long sbi_get_impl_version(void) {
    return sbi_ecall(SBI_EXT_BASE, 2, 0,0,0,0,0,0).value;
}

/* ── Shell commands ──────────────────────────────────────────────────────── */

/*
 * Bootloader staging address.
 * On QEMU  : 0x82000000  (2 MB above the bootloader at 0x80200000)
 * On board : 0x20000000  (512 MB above the bootloader at 0x00200000)
 */
#ifdef QEMU
#define LOAD_ADDR 0x82000000UL
#else
#define LOAD_ADDR 0x20000000UL
#endif

static void cmd_help(void) {
    uart_puts("Commands:\r\n"
              "  help          - this message-3\r\n"
              "  hello         - Hello World\r\n"
              "  info          - SBI/system info\r\n"
              "  ls            - list initrd files\r\n"
              "  cat <file>    - print initrd file\r\n"
              "  load          - receive kernel via UART and boot it\r\n");
}

static void cmd_hello(void) {
    uart_puts("Hello World!\r\n");
}

static void cmd_info(void) {
    uart_puts("SBI spec ver:  "); uart_hex((unsigned long)sbi_get_spec_version()); uart_puts("\r\n");
    uart_puts("SBI impl ID:   "); uart_hex((unsigned long)sbi_get_impl_id());      uart_puts("\r\n");
    uart_puts("SBI impl ver:  "); uart_hex((unsigned long)sbi_get_impl_version()); uart_puts("\r\n");
    uart_puts("DTB at:        "); uart_hex((unsigned long)g_fdt);           uart_puts("\r\n");
    if (g_initrd) {
        uart_puts("initrd at:     "); uart_hex((unsigned long)g_initrd);   uart_puts("\r\n");
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
 * cmd_load – receive a kernel image over UART and jump to it.
 *
 * Protocol (little-endian):
 *   [4 bytes] magic  = 0x544F4F42  ("BOOT")
 *   [4 bytes] size   = byte count of the kernel binary
 *   [size bytes]     kernel binary
 */
static void cmd_load(void) {
    uart_puts("Waiting for kernel (send BOOT header qq)...\r\n");

    unsigned long magic = uart_read_u32_le();
    if (magic != 0x544F4F42UL) {
        uart_puts("Bad magic, aborting\r\n");
        return;
    }
    unsigned long size = uart_read_u32_le();

    uart_puts("Receiving ");
    uart_putdec(size);
    uart_puts(" bytes to 0x");
    uart_hex(LOAD_ADDR);
    uart_puts("...\r\n");

    uint8_t *dst = (uint8_t *)LOAD_ADDR;
    for (unsigned long i = 0; i < size; i++)
        dst[i] = (uint8_t)uart_getc();

    uart_puts("Done. Jumping to 0x");
    uart_hex(LOAD_ADDR);
    uart_puts("\r\n");

    uart_init();

    jump_to_entry(LOAD_ADDR, saved_hart_id, (unsigned long)g_fdt);

    /* should never reach here */
    while (1) {}
}

/* ── Shell ───────────────────────────────────────────────────────────────── */

static void dispatch(char *cmd, char *arg) {
    if      (k_strcmp(cmd, "help")  == 0) cmd_help();
    else if (k_strcmp(cmd, "hello") == 0) cmd_hello();
    else if (k_strcmp(cmd, "info")  == 0) cmd_info();
    else if (k_strcmp(cmd, "ls")    == 0) cmd_ls();
    else if (k_strcmp(cmd, "cat")   == 0) cmd_cat(arg);
    else if (k_strcmp(cmd, "load")  == 0) cmd_load();
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
        uart_flush_rx();   /* drain QEMU PTY loopback bytes that arrived
                            * while the kernel was printing startup output */
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

    /* ── Initialize UART hardware first, before any output ── */
    uart_init();

    /* ── Basic Exercise 2: get UART base from devicetree ── */
    unsigned long uart_base = dtb_get_uart_base(fdt);
    if (uart_base) {
        uart_set_base(uart_base);
    }

    /* Separate our output from OpenSBI's with blank lines.
     * We deliberately avoid escape sequences here: PTY mode collects all
     * buffered bytes and delivers them to picocom at once, so screen-clear
     * sequences interact badly with the terminal rendering. */
    uart_puts("\r\n\r\n");

    uart_puts("OSC2026 Lab 2 - OrangePi RV2\r\n");
    uart_puts("DTB at "); uart_hex((unsigned long)fdt); uart_puts("\r\n");

    if (uart_base) {
        uart_puts("UART base from DTB: "); uart_hex(uart_base); uart_puts("\r\n");
    } else {
        uart_puts("UART base: DTB parse failed, using default\r\n");
    }

    /* ── Basic Exercise 3: locate initrd from devicetree ── */
    unsigned long initrd_start = dtb_get_initrd_start(fdt);
    if (initrd_start) {
        g_initrd = (const void *)initrd_start;
        uart_puts("initrd at "); uart_hex(initrd_start); uart_puts("\r\n");
    } else {
        uart_puts("No initrd found in DTB\r\n");
    }

    uart_puts("Type 'help' for commands.\r\n");

    shell_run();
}
