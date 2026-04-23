/*
 * Minimal FDT (Flattened Device Tree) parser.
 *
 * Provides:
 *   dtb_get_uart_base()    – read UART base from /soc/serial
 *   dtb_get_initrd_start() – read initrd start from /chosen
 */

typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      size_t;
typedef unsigned long      uintptr_t;

extern size_t k_strlen(const char *s);
extern int    k_strcmp(const char *a, const char *b);
extern int    k_strncmp(const char *a, const char *b, size_t n);

/* ── FDT token / magic constants ─────────────────────────────────────────── */
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

/* ── Byte-swap helpers (DTB is big-endian) ───────────────────────────────── */
uint32_t bswap32(uint32_t x) {
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

/* ── Core FDT traversal ───────────────────────────────────────────────────── */

/*
 * fdt_path_offset – locate a node by path, returns byte offset from fdt base.
 * Returns -1 on error.
 */
static int fdt_path_offset(const void *fdt, const char *path) {
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (bswap32(hdr->magic) != FDT_MAGIC)
        return -1;

    char buf[256];
    size_t plen = k_strlen(path);
    if (plen >= sizeof(buf)) return -1;
    for (size_t i = 0; i <= plen; i++) buf[i] = path[i];

    char *comps[32];
    int ncomp = 0;
    char *start = (buf[0] == '/') ? buf + 1 : buf;
    char *p = start;
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

/* ── Public DTB helpers ───────────────────────────────────────────────────── */

/* Read #address-cells from a node; defaults to 2 if absent. */
static int fdt_address_cells(const void *fdt, int nodeoffset) {
    int len;
    const uint32_t *p = (const uint32_t *)fdt_getprop(
                            fdt, nodeoffset, "#address-cells", &len);
    if (!p || len != 4) return 2;
    return (int)bswap32(*p);
}

/*
 * Read UART base address from /soc/serial reg property.
 * Reads #address-cells from /soc to handle both 32-bit (QEMU: cells=1)
 * and 64-bit (OrangePi RV2: cells=2) address encodings.
 */
unsigned long dtb_get_uart_base(const void *fdt) {
    int soc_off = fdt_path_offset(fdt, "/soc");
    int addr_cells = (soc_off >= 0) ? fdt_address_cells(fdt, soc_off) : 2;

    int off = fdt_path_offset(fdt, "/soc/serial");
    if (off < 0) return 0;
    int len;
    const uint32_t *reg =
        (const uint32_t *)fdt_getprop(fdt, off, "reg", &len);
    if (!reg) return 0;

    if (addr_cells == 1) {
        if (len < 4) return 0;
        return (unsigned long)bswap32(reg[0]);
    } else {
        if (len < 8) return 0;
        return ((unsigned long)bswap32(reg[0]) << 32)
             | (unsigned long)bswap32(reg[1]);
    }
}

/*
 * Read initrd start address from /chosen linux,initrd-start.
 * May be 4 bytes (u32) or 8 bytes (u64), both big-endian.
 */
unsigned long dtb_get_initrd_start(const void *fdt) {
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

unsigned long dtb_get_initrd_end(const void *fdt) {
    int off = fdt_path_offset(fdt, "/chosen");
    if (off < 0) return 0;
    int len;
    const void *prop =
        fdt_getprop(fdt, off, "linux,initrd-end", &len);
    if (!prop) return 0;
    if (len == 4) return bswap32(*(const uint32_t *)prop);
    if (len == 8) return bswap64(*(const uint64_t *)prop);
    return 0;
}

int dtb_get_memory_region0(const void *fdt, uint64_t *base, uint64_t *size) {
    int off = fdt_path_offset(fdt, "/memory");
    if (off < 0) return 0;

    int root_off = fdt_path_offset(fdt, "/");
    int ac = 2, sc = 2;
    if (root_off >= 0) {
        int len;
        const uint32_t *p = (const uint32_t *)fdt_getprop(fdt, root_off, "#address-cells", &len);
        if (p && len == 4) ac = (int)bswap32(*p);
        p = (const uint32_t *)fdt_getprop(fdt, root_off, "#size-cells", &len);
        if (p && len == 4) sc = (int)bswap32(*p);
    }
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2) return 0;

    int len;
    const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, off, "reg", &len);
    if (!reg) return 0;
    int cells = ac + sc;
    if (len < cells * 4) return 0;

    uint64_t b = (ac == 2) ? (((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]))
                           : (uint64_t)bswap32(reg[0]);
    uint64_t s = (sc == 2) ? (((uint64_t)bswap32(reg[ac]) << 32) | bswap32(reg[ac + 1]))
                           : (uint64_t)bswap32(reg[ac]);
    if (!s) return 0;
    *base = b;
    *size = s;
    return 1;
}

int dtb_get_reserved_region(const void *fdt, int idx, uint64_t *base, uint64_t *size) {
    int rsv_off = fdt_path_offset(fdt, "/reserved-memory");
    if (rsv_off < 0) return 0;

    int ac = 2, sc = 2;
    {
        int len;
        const uint32_t *p = (const uint32_t *)fdt_getprop(fdt, rsv_off, "#address-cells", &len);
        if (p && len == 4) ac = (int)bswap32(*p);
        p = (const uint32_t *)fdt_getprop(fdt, rsv_off, "#size-cells", &len);
        if (p && len == 4) sc = (int)bswap32(*p);
    }
    if (ac < 1 || ac > 2 || sc < 1 || sc > 2) return 0;

    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    const uint8_t *cur = (const uint8_t *)fdt + bswap32(hdr->off_dt_struct);
    uint32_t strs_off = bswap32(hdr->off_dt_strings);
    int depth = 0;
    int in_rsv = 0;
    int child_idx = -1;

    while (1) {
        uint32_t tok = bswap32(*(const uint32_t *)cur); cur += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)cur;
            cur = (const uint8_t *)align_up_ptr(cur + k_strlen(name) + 1, 4);
            if (!in_rsv) {
                depth++;
                if (depth == 1) {
                    /* nothing */
                } else if (depth >= 1) {
                    /* noop */
                }
                int node_off = (int)((const uint8_t *)cur - (const uint8_t *)fdt);
                (void)node_off;
            } else {
                depth++;
                if (depth == 2) child_idx++;
            }
            int node_start = (int)((const uint8_t *)cur - (const uint8_t *)fdt);
            if (!in_rsv && node_start - 8 == rsv_off) {
                in_rsv = 1;
                depth = 1;
                child_idx = -1;
            }
        } else if (tok == FDT_END_NODE) {
            if (in_rsv && depth == 1) { in_rsv = 0; depth = 0; }
            else if (depth > 0) depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t *)cur); cur += 4;
            uint32_t nameoff = bswap32(*(const uint32_t *)cur); cur += 4;
            const uint8_t *val = cur;
            cur = (const uint8_t *)align_up_ptr(cur + len, 4);
            if (in_rsv && depth == 2 && child_idx == idx) {
                const char *pname = (const char *)fdt + strs_off + nameoff;
                if (k_strcmp(pname, "reg") == 0) {
                    int cells = ac + sc;
                    if ((int)len < cells * 4) return 0;
                    const uint32_t *reg = (const uint32_t *)val;
                    uint64_t b = (ac == 2) ? (((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]))
                                           : (uint64_t)bswap32(reg[0]);
                    uint64_t s = (sc == 2) ? (((uint64_t)bswap32(reg[ac]) << 32) | bswap32(reg[ac + 1]))
                                           : (uint64_t)bswap32(reg[ac]);
                    *base = b; *size = s;
                    return s != 0;
                }
            }
        } else if (tok == FDT_NOP) {
        } else {
            break;
        }
    }
    return 0;
}

/*
 * Read timebase-frequency from /cpus node.
 * Returns 0 if not found.
 */
unsigned long dtb_get_timebase_freq(const void *fdt) {
    int off = fdt_path_offset(fdt, "/cpus");
    if (off < 0) return 0;
    int len;
    const uint32_t *p = (const uint32_t *)fdt_getprop(
                            fdt, off, "timebase-frequency", &len);
    if (!p) return 0;
    if (len == 4) return (unsigned long)bswap32(p[0]);
    if (len == 8)
        return ((unsigned long)bswap32(p[0]) << 32)
             | (unsigned long)bswap32(p[1]);
    return 0;
}
