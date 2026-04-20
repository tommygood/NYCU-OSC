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
  dump_free_lists();
  uart_puts("\n===== Part 1 =====\n");

  void *p1 = allocate(129);
  free(p1);

  uart_puts("\n=== Part 1 End ===\n");

  uart_puts("\n===== Part 2 =====\n");

  // Allocate all blocks at order 0, 1, 2 and 3
  int NUM_BLOCKS_AT_ORDER_0 = 0;  // Need modified
  int NUM_BLOCKS_AT_ORDER_1 = 0;
  int NUM_BLOCKS_AT_ORDER_2 = 0;
  int NUM_BLOCKS_AT_ORDER_3 = 0;

  void *ps0[NUM_BLOCKS_AT_ORDER_0];
  void *ps1[NUM_BLOCKS_AT_ORDER_1];
  void *ps2[NUM_BLOCKS_AT_ORDER_2];
  void *ps3[NUM_BLOCKS_AT_ORDER_3];
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i) {
      ps0[i] = allocate(4096);
  }
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i) {
      ps1[i] = allocate(8192);
  }
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i) {
      ps2[i] = allocate(16384);
  }
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i) {
      ps3[i] = allocate(32768);
  }

  uart_puts("\n-----------\n");

  long MAX_BLOCK_SIZE = PAGE_SIZE * (1 << MAX_ORDER);

  void *c1, *c2, *c3, *c4, *c5, *c6, *c7, *c8, *p2, *p3, *p4, *p5, *p6, *p7;

  p1 = allocate(4095);
  free(p1);                        // 4095
  p1 = allocate(4095);

  c1 = allocate(1000);
  c2 = allocate(1023);
  c3 = allocate(999);
  c4 = allocate(1010);
  free(c3);                        // 999
  c5 = allocate(989);  
  c3 = allocate(88);
  c6 = allocate(1001);
  free(c3);                        // 88
  c7 = allocate(2045);
  c8 = allocate(1);

  p2 = allocate(4096);
  free(c8);                        // 1
  p3 = allocate(16000);
  free(p1);                        // 4095
  free(c7);                        // 2045
  p4 = allocate(4097);
  p5 = allocate(MAX_BLOCK_SIZE + 1);
  p6 = allocate(MAX_BLOCK_SIZE);
  free(p2);                        // 4096
  free(p4);                        // 4097
  p7 = allocate(7197);

  free(p6);                        // MAX_BLOCK_SIZE
  free(p3);                        // 16000
  free(p7);                        // 7197
  free(c1);                        // 1000
  free(c6);                        // 1001
  free(c2);                        // 1023
  free(c5);                        // 989
  free(c4);                        // 1010


  uart_puts("\n-----------\n");

  // Free all blocks remaining
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i) {
      free(ps0[i]);
  }
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i) {
      free(ps1[i]);
  }
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i) {
      free(ps2[i]);
  }
  for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i) {
      free(ps3[i]);
  }

  uart_puts("\n=== Part 2 End ===\n");
}

static void cmd_mmtest1(void) {
    uart_puts("Testing memory allocation...\r\n");
    dump_free_lists();
    char *ptr1 = (char *)allocate(4000);
    char *ptr2 = (char *)allocate(8000);
    char *ptr3 = (char *)allocate(4000);
    char *ptr4 = (char *)allocate(4000);

    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);

    /* Test kmalloc */
    uart_puts("Testing dynamic allocator...\r\n");
    char *kmem_ptr1 = (char *)allocate(16);
    char *kmem_ptr2 = (char *)allocate(32);
    char *kmem_ptr3 = (char *)allocate(64);
    char *kmem_ptr4 = (char *)allocate(128);

    free(kmem_ptr1);
    free(kmem_ptr2);
    free(kmem_ptr3);
    free(kmem_ptr4);

    char *kmem_ptr5 = (char *)allocate(16);
    char *kmem_ptr6 = (char *)allocate(32);

    free(kmem_ptr5);
    free(kmem_ptr6);

    /* Test allocate new page if the cache is not enough */
    void *kmem_ptr[100];
    for (int i = 0; i < 100; i++)
        kmem_ptr[i] = (char *)allocate(128);
    for (int i = 0; i < 100; i++)
        free(kmem_ptr[i]);

    /* Test exceeding the maximum size */
    char *kmem_ptr7 = (char *)allocate(MAX_ALLOC_SIZE + 1);
    if (kmem_ptr7 == 0) {
        uart_puts("Allocation failed as expected for size > MAX_ALLOC_SIZE\r\n");
    } else {
        uart_puts("Unexpected allocation success for size > MAX_ALLOC_SIZE\r\n");
        free(kmem_ptr7);
    }
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
