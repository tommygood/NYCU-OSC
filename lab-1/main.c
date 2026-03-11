/* Lab 1: Hello World - OrangePi RV2 bare-metal shell */

extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);

/* ── SBI interface ─────────────────────────────────────────────────────── */

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_ecall(int ext, int fid,
                        unsigned long arg0,
                        unsigned long arg1,
                        unsigned long arg2,
                        unsigned long arg3,
                        unsigned long arg4,
                        unsigned long arg5) {
    struct sbiret ret;
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = (unsigned long)fid;
    register unsigned long a7 asm("a7") = (unsigned long)ext;
    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");
    ret.error = a0;
    ret.value = a1;
    return ret;
}

#define SBI_EXT_BASE              0x10
#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_GET_IMP_ID       1
#define SBI_BASE_GET_IMP_VERSION  2

static long sbi_get_spec_version(void) {
    return sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_SPEC_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

static long sbi_get_impl_id(void) {
    return sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_IMP_ID,
                     0, 0, 0, 0, 0, 0).value;
}

static long sbi_get_impl_version(void) {
    return sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_IMP_VERSION,
                     0, 0, 0, 0, 0, 0).value;
}

/* ── Shell ─────────────────────────────────────────────────────────────── */

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void cmd_help(void) {
    uart_puts("Available commands:\r\n"
              "  help   - Show this help\r\n"
              "  hello  - Print Hello World!\r\n"
              "  info   - Show system information\r\n");
}

static void cmd_hello(void) {
    uart_puts("Hello World!\r\n");
}

static void cmd_info(void) {
    uart_puts("OpenSBI spec version:  ");
    uart_hex((unsigned long)sbi_get_spec_version());
    uart_puts("\r\nImplementation ID:     ");
    uart_hex((unsigned long)sbi_get_impl_id());
    uart_puts("\r\nImplementation ver:    ");
    uart_hex((unsigned long)sbi_get_impl_version());
    uart_puts("\r\n");
}

static void dispatch(char *cmd) {
    if (streq(cmd, "help"))       cmd_help();
    else if (streq(cmd, "hello")) cmd_hello();
    else if (streq(cmd, "info"))  cmd_info();
    else if (cmd[0] != '\0') {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\r\n");
    }
}

void start_kernel(void) {
    uart_puts("\r\nOSC2026 Lab 1 - OrangePi RV2\r\n"
              "Type 'help' for available commands.\r\n");

    char buf[128];
    int pos;

    while (1) {
        uart_puts("\r\n$ ");
        pos = 0;

        while (1) {
            char c = uart_getc();

            if (c == '\n') {          /* Enter (uart_getc converts \r→\n) */
                uart_puts("\r\n");
                buf[pos] = '\0'; // add a null terminator at the end to compare if strs are equal
                dispatch(buf);
                break;
            } else if (c == 0x7f) {   /* Backspace */
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
