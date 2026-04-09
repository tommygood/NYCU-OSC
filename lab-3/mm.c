typedef unsigned char      uint8_t;
typedef unsigned int       uint32_t;
typedef unsigned long      size_t;
typedef unsigned long      uintptr_t;
typedef unsigned long long uint64_t;

extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_putdec(unsigned long n);

#include "mm.h"

extern int dtb_get_memory_region0(const void *fdt, uint64_t *base, uint64_t *size);
extern unsigned long dtb_get_initrd_start(const void *fdt);
extern unsigned long dtb_get_initrd_end(const void *fdt);
extern int dtb_get_reserved_region(const void *fdt, int idx, uint64_t *base, uint64_t *size);

extern char _start, _end;

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

struct frame {
    int order;
    int is_free;
    int is_head;
    int is_reserved;
    int alloc_order;
    int pool_idx;
    struct list_head node;
};

struct chunk {
    struct chunk *next;
};

struct pool {
    size_t chunk_size;
    struct chunk *free_list;
};

struct large_hdr {
    uint32_t magic;
    uint32_t order;
};

#define LARGE_MAGIC 0x4C415247U /* LARG */

static struct list_head g_free_area[MAX_ORDER + 1];
static struct frame *g_frames;
static uint8_t *g_startup_cur;
static uint8_t *g_startup_end;
static uint64_t g_mem_base;
static uint64_t g_mem_size;
static size_t g_frame_count;
static int g_mm_ready;
static int g_log_runtime = 1;

static const size_t g_pool_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
static struct pool g_pools[sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0])];

static void list_init(struct list_head *h) { h->next = h->prev = h; }
static int list_empty(struct list_head *h) { return h->next == h; }
static void list_add(struct list_head *h, struct list_head *n) {
    n->next = h->next; n->prev = h; h->next->prev = n; h->next = n;
}
static void list_del(struct list_head *n) {
    n->prev->next = n->next; n->next->prev = n->prev; n->next = n->prev = n;
}

static size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

static uintptr_t idx_to_pa(size_t idx) { return (uintptr_t)(g_mem_base + (idx << PAGE_SHIFT)); }
static size_t pa_to_idx(uintptr_t pa) { return (size_t)((pa - (uintptr_t)g_mem_base) >> PAGE_SHIFT); }
static size_t buddy_of(size_t idx, int order) { return idx ^ (1UL << order); }

static void log_add(size_t idx, int order) {
    if (!g_log_runtime) return;
    uart_puts("[+] Add page "); uart_putdec(idx); uart_puts(" to order "); uart_putdec(order); uart_puts("\r\n");
}
static void log_del(size_t idx, int order) {
    if (!g_log_runtime) return;
    uart_puts("[-] Remove page "); uart_putdec(idx); uart_puts(" from order "); uart_putdec(order); uart_puts("\r\n");
}
static void log_split(size_t idx, int from, int to) {
    uart_puts("[*] Split page "); uart_putdec(idx); uart_puts(" order "); uart_putdec(from);
    uart_puts(" -> "); uart_putdec(to); uart_puts("\r\n");
}
static void log_merge(size_t a, size_t b, int order) {
    uart_puts("[*] Merge page "); uart_putdec(a); uart_puts(" with buddy "); uart_putdec(b);
    uart_puts(" at order "); uart_putdec(order); uart_puts("\r\n");
}

static void memory_reserve_startup(uint64_t start, uint64_t size) {
    if (!size || !g_startup_cur) return;
    uintptr_t s = (uintptr_t)start;
    uintptr_t e = s + (uintptr_t)size;
    uintptr_t c = (uintptr_t)g_startup_cur;
    uintptr_t n = (uintptr_t)g_startup_end;
    if (e <= c || s >= n) return;
    if (s <= c && e > c) g_startup_cur = (uint8_t *)align_up((size_t)e, PAGE_SIZE);
}

static void *startup_alloc(size_t size, size_t align) {
    uintptr_t cur = (uintptr_t)g_startup_cur;
    uintptr_t end = (uintptr_t)g_startup_end;
    cur = align_up(cur, align);
    if (cur + size > end) return 0;
    g_startup_cur = (uint8_t *)(cur + size);
    return (void *)cur;
}

static void frame_mark_reserved(size_t idx) {
    if (idx >= g_frame_count) return;
    g_frames[idx].is_reserved = 1;
    g_frames[idx].is_free = 0;
}

static void frame_add_free(size_t idx, int order) {
    struct frame *f = &g_frames[idx];
    f->order = order; f->is_head = 1; f->is_free = 1; f->alloc_order = -1; f->pool_idx = -1;
    list_add(&g_free_area[order], &f->node);
    log_add(idx, order);
}

static void frame_remove_free(size_t idx, int order) {
    struct frame *f = &g_frames[idx];
    list_del(&f->node);
    f->is_free = 0;
    log_del(idx, order);
}

static void buddy_build_from_free_frames(void) {
    size_t i = 0;
    while (i < g_frame_count) {
        if (g_frames[i].is_reserved) { i++; continue; }
        int order = MAX_ORDER;
        while (order > 0) {
            size_t blk = (1UL << order);
            // i must be aligned with current order
            if ((i & (blk - 1)) == 0 && i + blk <= g_frame_count) {
                int ok = 1;
                for (size_t j = i; j < i + blk; j++) {
                    if (g_frames[j].is_reserved) { ok = 0; break; }
                }
                if (ok) break;
            }
            order--;
        }
        frame_add_free(i, order);
        i += (1UL << order);
    }
}

static void memory_reserve(uint64_t start, uint64_t size) {
    if (!size || !g_frames) return;
    uint64_t end = start + size;
    uint64_t mem_end = g_mem_base + g_mem_size;
    if (end <= g_mem_base || start >= mem_end) return;
    if (start < g_mem_base) start = g_mem_base;
    if (end > mem_end) end = mem_end;
    size_t sidx = pa_to_idx((uintptr_t)(start & ~(PAGE_SIZE - 1)));
    size_t eidx = pa_to_idx((uintptr_t)align_up((size_t)end, PAGE_SIZE));
    if (eidx > g_frame_count) eidx = g_frame_count;
    for (size_t i = sidx; i < eidx; i++) frame_mark_reserved(i);
    uart_puts("[Reserve] ["); uart_hex((unsigned long)start); uart_puts(", "); uart_hex((unsigned long)end); uart_puts(")\r\n");
}

static int page_order_for_size(size_t size) {
    size_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    int order = 0;
    size_t blk = 1;
    while (blk < pages) { blk <<= 1; order++; }
    return order;
}

static void *page_alloc_order(int req_order) {
    if (req_order < 0 || req_order > MAX_ORDER) return 0;
    int order = req_order;
    while (order <= MAX_ORDER && list_empty(&g_free_area[order])) order++;
    if (order > MAX_ORDER) return 0;

    struct frame *f = (struct frame *)((char *)g_free_area[order].next - (size_t)&((struct frame *)0)->node);
    size_t idx = (size_t)(f - g_frames);
    frame_remove_free(idx, order);

    while (order > req_order) {
        order--;
        size_t bidx = idx + (1UL << order);
        g_frames[bidx].order = order;
        g_frames[bidx].is_head = 1;
        g_frames[bidx].is_free = 1;
        g_frames[bidx].alloc_order = -1;
        g_frames[bidx].pool_idx = -1;
        list_add(&g_free_area[order], &g_frames[bidx].node);
        log_split(idx, order + 1, order);
        log_add(bidx, order);
    }
    g_frames[idx].order = req_order;
    g_frames[idx].is_head = 1;
    g_frames[idx].is_free = 0;
    g_frames[idx].alloc_order = req_order;
    g_frames[idx].pool_idx = -1;
    uintptr_t pa = idx_to_pa(idx);
    uart_puts("[Page] Allocate "); uart_hex(pa); uart_puts(" order "); uart_putdec(req_order); uart_puts("\r\n");
    return (void *)pa;
}

static void page_free_order(void *ptr, int order) {
    if (!ptr) return;
    size_t idx = pa_to_idx((uintptr_t)ptr);
    if (idx >= g_frame_count) return;

    while (order < MAX_ORDER) {
        size_t bidx = buddy_of(idx, order);
        if (bidx >= g_frame_count) break;
        struct frame *bf = &g_frames[bidx];
        if (!(bf->is_head && bf->is_free && bf->order == order && !bf->is_reserved)) break;
        frame_remove_free(bidx, order);
        if (bidx < idx) idx = bidx;
        log_merge(idx, bidx, order);
        order++;
    }
    frame_add_free(idx, order);
    g_frames[idx].alloc_order = -1;
    g_frames[idx].pool_idx = -1;
    uart_puts("[Page] Free "); uart_hex(idx_to_pa(idx)); uart_puts(" order "); uart_putdec(order); uart_puts("\r\n");
}

static int pool_find(size_t size) {
    for (size_t i = 0; i < sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]); i++) {
        if (size <= g_pool_sizes[i]) return (int)i;
    }
    return -1;
}

static void pool_refill(int pidx) {
    void *page = page_alloc_order(0);
    if (!page) return;
    size_t idx = pa_to_idx((uintptr_t)page);
    g_frames[idx].pool_idx = pidx;

    size_t csz = g_pools[pidx].chunk_size;
    uint8_t *p = (uint8_t *)page;
    size_t n = PAGE_SIZE / csz;
    for (size_t i = 0; i < n; i++) {
        struct chunk *c = (struct chunk *)(void *)(p + i * csz);
        c->next = g_pools[pidx].free_list;
        g_pools[pidx].free_list = c;
    }
}

int mm_init(const void *fdt) {
    uint64_t mem_base = 0, mem_size = 0;
    if (!dtb_get_memory_region0(fdt, &mem_base, &mem_size)) {
      return 1;
    }

    g_mem_base = mem_base;
    g_mem_size = mem_size;
    g_frame_count = (size_t)(mem_size >> PAGE_SHIFT);

    for (int i = 0; i <= MAX_ORDER; i++) list_init(&g_free_area[i]);
    for (size_t i = 0; i < sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]); i++) {
        g_pools[i].chunk_size = g_pool_sizes[i];
        g_pools[i].free_list = 0;
    }

    /* Start bump pointer directly after the kernel image, skipping the
     * low memory region (OpenSBI + original kernel load area 0x0~_end). */
    uintptr_t after_kernel = align_up((size_t)(uintptr_t)&_end, PAGE_SIZE);
    g_startup_cur = (uint8_t *)after_kernel;
    g_startup_end = (uint8_t *)(uintptr_t)(mem_base + mem_size);
    memory_reserve_startup((uint64_t)(uintptr_t)&_start, (uint64_t)((uintptr_t)&_end - (uintptr_t)&_start));
    memory_reserve_startup((uint64_t)(uintptr_t)fdt, 0x10000); /* conservative before parsing header */
    {
        unsigned long rs = dtb_get_initrd_start(fdt);
        unsigned long re = dtb_get_initrd_end(fdt);
        if (rs && re && re > rs) memory_reserve_startup((uint64_t)rs, (uint64_t)(re - rs));
    }

    g_frames = (struct frame *)startup_alloc(g_frame_count * sizeof(struct frame), PAGE_SIZE);
    if (!g_frames) return -1;
    for (size_t i = 0; i < g_frame_count; i++) {
        g_frames[i].order = 0; g_frames[i].is_free = 0; g_frames[i].is_head = 0;
        g_frames[i].is_reserved = 0; g_frames[i].alloc_order = -1; g_frames[i].pool_idx = -1;
        list_init(&g_frames[i].node);
    }

    memory_reserve((uint64_t)(uintptr_t)&_start, (uint64_t)((uintptr_t)&_end - (uintptr_t)&_start));
    {
        struct fdt_header { uint32_t magic, totalsize; };
        const struct fdt_header *h = (const struct fdt_header *)fdt;
        uint32_t totalsize = ((h->totalsize & 0x000000ffU) << 24) | ((h->totalsize & 0x0000ff00U) << 8)
                           | ((h->totalsize & 0x00ff0000U) >> 8) | ((h->totalsize & 0xff000000U) >> 24);
        if (totalsize) memory_reserve((uint64_t)(uintptr_t)fdt, totalsize);
    }
    {
        unsigned long rs = dtb_get_initrd_start(fdt), re = dtb_get_initrd_end(fdt);
        if (rs && re && re > rs) memory_reserve((uint64_t)rs, (uint64_t)(re - rs));
    }
    for (int i = 0; i < 32; i++) {
        uint64_t rb, rsz;
        if (!dtb_get_reserved_region(fdt, i, &rb, &rsz)) break;
        memory_reserve(rb, rsz);
    }
    memory_reserve((uint64_t)(uintptr_t)g_frames, g_frame_count * sizeof(struct frame));

    g_log_runtime = 0;
    buddy_build_from_free_frames();
    g_log_runtime = 1;
    g_mm_ready = 1;
    uart_puts("[MM] initialized\r\n");
    return 0;
}

void *alloc(size_t size) {
    if (!g_mm_ready) return 0;
    if (size == 0 || size > MAX_ALLOC_SIZE) return 0;

    if (size >= PAGE_SIZE) {
        // use page frame allocator
        int order = page_order_for_size(size + sizeof(struct large_hdr));
        void *page = page_alloc_order(order);
        if (!page) return 0;
        struct large_hdr *h = (struct large_hdr *)page;
        h->magic = LARGE_MAGIC;
        h->order = (uint32_t)order;
        return (void *)((uint8_t *)page + sizeof(struct large_hdr));
    }

    int pidx = pool_find(size);
    if (pidx < 0) {
        /* No fitting small pool (e.g. 2049..4095): fallback to page allocator. */
        int order = page_order_for_size(size + sizeof(struct large_hdr));
        void *page = page_alloc_order(order);
        if (!page) return 0;
        struct large_hdr *h = (struct large_hdr *)page;
        h->magic = LARGE_MAGIC;
        h->order = (uint32_t)order;
        return (void *)((uint8_t *)page + sizeof(struct large_hdr));
    }
    if (!g_pools[pidx].free_list) pool_refill(pidx);
    if (!g_pools[pidx].free_list) return 0;

    struct chunk *c = g_pools[pidx].free_list;
    g_pools[pidx].free_list = c->next;
    uart_puts("[Chunk] Allocate "); uart_hex((unsigned long)(uintptr_t)c);
    uart_puts(" size "); uart_putdec(g_pools[pidx].chunk_size); uart_puts("\r\n");
    return (void *)c;
}

void free(void *ptr) {
    if (!ptr || !g_mm_ready) return;
    uintptr_t pa = (uintptr_t)ptr;
    size_t pidx = pa_to_idx(pa & ~(PAGE_SIZE - 1));
    if (pidx >= g_frame_count) return;

    struct large_hdr *h = (struct large_hdr *)(void *)(pa - sizeof(struct large_hdr));
    if ((pa & (PAGE_SIZE - 1)) >= sizeof(struct large_hdr) && h->magic == LARGE_MAGIC && h->order <= MAX_ORDER) {
        page_free_order((void *)((uintptr_t)h & ~(PAGE_SIZE - 1)), (int)h->order);
        return;
    }

    int pool = g_frames[pidx].pool_idx;
    if (pool < 0 || pool >= (int)(sizeof(g_pools) / sizeof(g_pools[0]))) return;
    struct chunk *c = (struct chunk *)ptr;
    c->next = g_pools[pool].free_list;
    g_pools[pool].free_list = c;
    uart_puts("[Chunk] Free "); uart_hex((unsigned long)(uintptr_t)c);
    uart_puts(" size "); uart_putdec(g_pools[pool].chunk_size); uart_puts("\r\n");
}
