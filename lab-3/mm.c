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
extern uint32_t bswap32(uint32_t x);

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
static size_t buddy_of(size_t idx, int order) { return idx ^ (1UL << order); } // flip the order th bit of page index to get buddy index

static void log_add(size_t idx, int order) {
    if (!g_log_runtime) return;
    size_t end = idx + (1UL << order) - 1;
    uart_puts("[+] Add page "); uart_putdec(idx); uart_puts(" to order "); uart_putdec(order);
    uart_puts(". Range of pages: ["); uart_putdec(idx); uart_puts(", "); uart_putdec(end); uart_puts("]\r\n");
}
static void log_del(size_t idx, int order) {
    if (!g_log_runtime) return;
    size_t end = idx + (1UL << order) - 1;
    uart_puts("[-] Remove page "); uart_putdec(idx); uart_puts(" from order "); uart_putdec(order);
    uart_puts(". Range of pages: ["); uart_putdec(idx); uart_puts(", "); uart_putdec(end); uart_puts("]\r\n");
}
static void log_split(size_t idx, int from, int to) {
    uart_puts("[*] Split page "); uart_putdec(idx); uart_puts(" order "); uart_putdec(from);
    uart_puts(" -> "); uart_putdec(to); uart_puts("\r\n");
}
static void log_merge(size_t a, size_t b, int order) {
    uart_puts("[*] Buddy found! buddy idx: "); uart_putdec(b); uart_puts(" for page "); uart_putdec(a);
    uart_puts(" with order "); uart_putdec(order); uart_puts("\r\n");
}

void dump_free_lists(void) {
    uart_puts("--- Free lists ---\r\n");
    for (int i = MAX_ORDER; i >= 0; i--) {
        size_t count = 0;
        struct list_head *n = g_free_area[i].next;
        while (n != &g_free_area[i]) { count++; n = n->next; }
        uart_puts("  order "); uart_putdec(i); uart_puts(": "); uart_putdec(count); uart_puts(" block(s)\r\n");
    }
}

static void memory_reserve_startup(uint64_t start, uint64_t size) {
    if (!size || !g_startup_cur) return;
    uintptr_t s = (uintptr_t)start;
    uintptr_t e = s + (uintptr_t)size;
    uintptr_t c = (uintptr_t)g_startup_cur;
    uintptr_t n = (uintptr_t)g_startup_end;
    if (e <= c || s >= n) return;
    if (s <= c && e > c) g_startup_cur = (uint8_t *)align_up((size_t)e, PAGE_SIZE);
    if (s > c && s < n) g_startup_end = (uint8_t *)((uintptr_t)s & ~(PAGE_SIZE - 1));
}

static void *startup_alloc(size_t size, size_t align) {
    uintptr_t cur = (uintptr_t)g_startup_cur;
    uintptr_t end = (uintptr_t)g_startup_end;
    cur = align_up(cur, align); // align with page to prevent from mem wasting
    if (cur + size > end) return 0;
    g_startup_cur = (uint8_t *)(cur + size);
    return (void *)cur;
}

static void frame_mark_reserved(size_t idx) {
    if (idx >= g_frame_count) return;
    g_frames[idx].is_reserved = 1;
    g_frames[idx].is_free = 0;
}

// add the frame to block of specific order
static void frame_add_free(size_t idx, int order) {
    struct frame *f = &g_frames[idx];
    f->order = order; f->is_head = 1; f->is_free = 1; f->alloc_order = -1; f->pool_idx = -1;
    list_add(&g_free_area[order], &f->node); // O(1)
    log_add(idx, order);
}

// remove the frame from block of specific order
static void frame_remove_free(size_t idx, int order) {
    struct frame *f = &g_frames[idx];
    list_del(&f->node); // O(1) removal
    f->is_free = 0;
    log_del(idx, order);
}

static void buddy_build_from_free_frames(void) {
    size_t i = 0;
    while (i < g_frame_count) {
        if (g_frames[i].is_reserved) { i++; continue; }
        // try to build the block in higher order
        int order = MAX_ORDER;
        while (order > 0) {
            size_t blk = (1UL << order);
            // i must be aligned with current order, since we use flip index of ORDER to find buddy
            if ((i & (blk - 1)) == 0 && i + blk <= g_frame_count) {
                int ok = 1;
                for (size_t j = i; j < i + blk; j++) {
                    if (g_frames[j].is_reserved) { ok = 0; break; }
                }
                if (ok) break;
            }
            order--;
        }
        // add a block to order linked list
        frame_add_free(i, order);
        i += (1UL << order);
    }
}

// mark the frame we need to reserve
static void memory_reserve(uint64_t start, uint64_t size, const char *name) {
    if (!size || !g_frames) return;
    uint64_t end = start + size;
    uint64_t mem_end = g_mem_base + g_mem_size;
    if (end <= g_mem_base || start >= mem_end) return;
    // we can only modify the memory region
    if (start < g_mem_base) start = g_mem_base;
    if (end > mem_end) end = mem_end;

    // round start down
    size_t sidx = pa_to_idx((uintptr_t)(start & ~(PAGE_SIZE - 1)));
    // round end up
    size_t eidx = pa_to_idx((uintptr_t)align_up((size_t)end, PAGE_SIZE));

    if (eidx > g_frame_count) eidx = g_frame_count;

    for (size_t i = sidx; i < eidx; i++) frame_mark_reserved(i);
    uart_puts("[Reserve] "); uart_puts(name); uart_puts(": ["); uart_hex((unsigned long)start); uart_puts(", "); uart_hex((unsigned long)end);
    uart_puts("). Range of pages: ["); uart_putdec(sidx); uart_puts(", "); uart_putdec(eidx); uart_puts(")\r\n");
}

static int page_order_for_size(size_t size) {
    // find the min page nums we need for this size
    size_t pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    int order = 0;
    size_t blk = 1;
    while (blk < pages) { blk <<= 1; order++; }
    return order;
}

static void *page_alloc_order(int req_order) {
    if (req_order < 0 || req_order > MAX_ORDER) return 0;
    int order = req_order;

    // find the min available order we can offer for request order
    while (order <= MAX_ORDER && list_empty(&g_free_area[order])) order++;
    if (order > MAX_ORDER) return 0;

    // get first available frame from free list on this order, then remove it from that order
    struct frame *f = (struct frame *)((char *)g_free_area[order].next - (size_t)&((struct frame *)0)->node);
    size_t idx = (size_t)(f - g_frames);
    frame_remove_free(idx, order);

    // split the page to next order with buddies when order > request order
    while (order > req_order) {
        order--;
        size_t bidx = idx + (1UL << order); // buddy index
        // modify metadata of buddy frame
        g_frames[bidx].order = order;
        g_frames[bidx].is_head = 1;
        g_frames[bidx].is_free = 1;
        g_frames[bidx].alloc_order = -1;
        g_frames[bidx].pool_idx = -1;

        // move buddy to head of next smaller order's linked list
        list_add(&g_free_area[order], &g_frames[bidx].node);

        log_split(idx, order + 1, order);
        log_add(bidx, order);
    }

    // mark this frame is used
    g_frames[idx].order = req_order;
    g_frames[idx].is_head = 1;
    g_frames[idx].is_free = 0;
    g_frames[idx].alloc_order = req_order;
    g_frames[idx].pool_idx = -1;
    uintptr_t pa = idx_to_pa(idx);

    uart_puts("[Page] Allocate "); uart_hex(pa); uart_puts(" at order "); uart_putdec(req_order);
    uart_puts(", page "); uart_putdec(idx); uart_puts("\r\n");
    dump_free_lists();
    return (void *)pa;
}

static void page_free_order(void *ptr, int order) {
    if (!ptr) return;
    size_t idx = pa_to_idx((uintptr_t)ptr);
    if (idx >= g_frame_count) return;

    // try to add the blocks in lower order to higher order
    while (order < MAX_ORDER) {
        // find the index of buudy
        size_t bidx = buddy_of(idx, order);
        if (bidx >= g_frame_count) break;
        struct frame *bf = &g_frames[bidx];

        // merge buddy if it's free and on block of same order
        if (!(bf->is_head && bf->is_free && bf->order == order && !bf->is_reserved)) break;
        frame_remove_free(bidx, order);
        log_merge(idx, bidx, order);
        if (bidx < idx) idx = bidx; // merge pages as [4,5], not [5,4]
        order++;
    }

    // add the page to max order of free area we can merge
    frame_add_free(idx, order);
    g_frames[idx].alloc_order = -1;
    g_frames[idx].pool_idx = -1;
    uart_puts("[Page] Free "); uart_hex(idx_to_pa(idx)); uart_puts(" and add back to order "); uart_putdec(order);
    uart_puts(", page "); uart_putdec(idx); uart_puts("\r\n");
    dump_free_lists();
}

static int pool_find(size_t size) {
    // find the chunk which size is fit in size 
    for (size_t i = 0; i < sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]); i++) {
        if (size <= g_pool_sizes[i]) return (int)i;
    }
    return -1;
}

static void pool_refill(int pidx) {
    // fill the chunk with a page
    void *page = page_alloc_order(0); // require a page from order 0
    if (!page) return;
    size_t idx = pa_to_idx((uintptr_t)page);
    g_frames[idx].pool_idx = pidx; // mark this frame is used by a pool

    size_t csz = g_pools[pidx].chunk_size;
    uint8_t *p = (uint8_t *)page;
    size_t n = PAGE_SIZE / csz; // number of chunks splited by a page

    // add n chunks to pool of pidx
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

    // we have to set the next and prev of a linked list node to itself when init
    // this could be used by subsequent usage of loop the linked list
    for (int i = 0; i <= MAX_ORDER; i++) list_init(&g_free_area[i]);

    // init the pool of physical memory allocator
    for (size_t i = 0; i < sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]); i++) {
        g_pools[i].chunk_size = g_pool_sizes[i];
        g_pools[i].free_list = 0;
    }

    /* Start bump pointer directly after the kernel image, skipping the
     * low memory region (OpenSBI + original kernel load area 0x0~_end). */
    uintptr_t after_kernel = align_up((size_t)(uintptr_t)&_end, PAGE_SIZE);
    g_startup_cur = (uint8_t *)after_kernel;
    g_startup_end = (uint8_t *)(uintptr_t)(mem_base + mem_size);
  
    // resever memory for fdt
    memory_reserve_startup((uint64_t)(uintptr_t)fdt, 0x10000); /* conservative before parsing header */

    // reserve memory for init ram disk
    {
        unsigned long rs = dtb_get_initrd_start(fdt);
        unsigned long re = dtb_get_initrd_end(fdt);
        if (rs && re && re > rs) memory_reserve_startup((uint64_t)rs, (uint64_t)(re - rs));
    }

    // reserve firmware reserved-memory regions from DTB
    for (int i = 0; ; i++) {
        uint64_t rb, rsz;
        if (!dtb_get_reserved_region(fdt, i, &rb, &rsz)) break;
        memory_reserve_startup(rb, rsz);
    }

    g_frames = (struct frame *)startup_alloc(g_frame_count * sizeof(struct frame), PAGE_SIZE);
    if (!g_frames) return -1;
    for (size_t i = 0; i < g_frame_count; i++) {
        g_frames[i].order = 0; g_frames[i].is_free = 0; g_frames[i].is_head = 0;
        g_frames[i].is_reserved = 0; g_frames[i].alloc_order = -1; g_frames[i].pool_idx = -1;
        list_init(&g_frames[i].node);
    }
    
    // marks pages as reserved so buddy_build_from_free_frames will skip them.
    
    // reserve pages for kernel
    memory_reserve((uint64_t)(uintptr_t)&_start, (uint64_t)((uintptr_t)&_end - (uintptr_t)&_start), "Kernel");

    // reserve pages for fdt
    {
        uint32_t totalsize = bswap32(((const uint32_t *)fdt)[1]);
        if (totalsize) memory_reserve((uint64_t)(uintptr_t)fdt, totalsize, "DTB");
    }

    // reserve pages for init ram disk
    {
        unsigned long rs = dtb_get_initrd_start(fdt), re = dtb_get_initrd_end(fdt);
        if (rs && re && re > rs) memory_reserve((uint64_t)rs, (uint64_t)(re - rs), "Initramfs");
    }

    // reserve pages for reserved region
    for (int i = 0; ; i++) {
        uint64_t rb, rsz;
        if (!dtb_get_reserved_region(fdt, i, &rb, &rsz)) break;
        memory_reserve(rb, rsz, "DTB reserved-memory");
    }

    // reserve pages for g_frames
    memory_reserve((uint64_t)(uintptr_t)g_frames, g_frame_count * sizeof(struct frame), "Frame array");

    g_log_runtime = 0;
    // build array of g_free_area which contains multiple orders of blocks for buddy system
    buddy_build_from_free_frames();
    g_log_runtime = 1;

    g_mm_ready = 1;
    uart_puts("[MM] initialized\r\n");
    return 0;
}

// allocate page or chunk based on given size
void *allocate(size_t size) {
    if (!g_mm_ready) return 0;
    if (size == 0 || size > MAX_ALLOC_SIZE) return 0;

    // use page frame allocator for large sizes or sizes that don't fit any pool
    int pidx = pool_find(size);
    if (size >= PAGE_SIZE || pidx < 0) {
        int order = page_order_for_size(size);
        void *page = page_alloc_order(order);
        return page; // use page on specific block
    }

    // refill the pool if there's no available chunk on this pool 
    if (!g_pools[pidx].free_list) pool_refill(pidx);
    if (!g_pools[pidx].free_list) return 0;

    // use a chunk from top of this pool
    struct chunk *c = g_pools[pidx].free_list;
    g_pools[pidx].free_list = c->next;

    uart_puts("[Chunk] Allocate "); uart_hex((unsigned long)(uintptr_t)c);
    uart_puts(" at chunk size "); uart_putdec(g_pools[pidx].chunk_size); uart_puts("\r\n");
    return (void *)c;
}

// free page or chunk
void free(void *ptr) {
    if (!ptr || !g_mm_ready) return;
    uintptr_t pa = (uintptr_t)ptr;

    // find the index of page that this ptr belongs to
    size_t pidx = pa_to_idx(pa & ~(PAGE_SIZE - 1));
    if (pidx >= g_frame_count) return;

    int pool = g_frames[pidx].pool_idx;
    if (pool < 0) {
        // free a block to page frame allocator
        int order = g_frames[pidx].alloc_order;
        if (order >= 0) page_free_order(ptr, order);
        return;
    }

    if (pool >= (int)(sizeof(g_pools) / sizeof(g_pools[0]))) return;

    // free a chunk, add back to the pool
    struct chunk *c = (struct chunk *)ptr;
    c->next = g_pools[pool].free_list;
    g_pools[pool].free_list = c;

    uart_puts("[Chunk] Free "); uart_hex((unsigned long)(uintptr_t)c);
    uart_puts(" at chunk size "); uart_putdec(g_pools[pool].chunk_size); uart_puts("\r\n");
}
