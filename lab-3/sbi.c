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
