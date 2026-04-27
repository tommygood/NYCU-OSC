extern void uart_puts(const char *s);
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

long sbi_get_spec_version(void) {
    return sbi_ecall(SBI_EXT_BASE, 0, 0,0,0,0,0,0).value;
}
long sbi_get_impl_id(void) {
    return sbi_ecall(SBI_EXT_BASE, 1, 0,0,0,0,0,0).value;
}
long sbi_get_impl_version(void) {
    return sbi_ecall(SBI_EXT_BASE, 2, 0,0,0,0,0,0).value;
}

/* SBI Timer Extension (EID = 0x54494D45 "TIME", FID = 0) */
#define SBI_EXT_TIME  0x54494D45

/* SBI Legacy set_timer (EID = 0x00) */
#define SBI_LEGACY_SET_TIMER  0x00

/* Probe if an SBI extension is available (base ext, fid=3) */
long sbi_probe_extension(long ext_id) {
    return sbi_ecall(SBI_EXT_BASE, 3, (unsigned long)ext_id, 0,0,0,0,0).value;
}

static int use_legacy_timer = 0;

void sbi_set_timer(unsigned long stime_value) {
    // function id of sbi_set_timer is 0
    if (use_legacy_timer)
        sbi_ecall(SBI_LEGACY_SET_TIMER, 0, stime_value, 0, 0, 0, 0, 0);
    else
        sbi_ecall(SBI_EXT_TIME, 0, stime_value, 0, 0, 0, 0, 0);
}

void sbi_timer_init(void) {
    if (sbi_probe_extension(SBI_EXT_TIME)) {
        use_legacy_timer = 0;
        uart_puts("[sbi] using new timer extension\r\n");
    }
    else
        use_legacy_timer = 1;
}
