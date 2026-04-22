typedef unsigned long size_t;

#define PAGE_SHIFT      12UL
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define MAX_ORDER       10
#define MAX_ALLOC_SIZE  2147483647UL

int   mm_init(const void *fdt);
void *allocate(size_t size);
void  free(void *ptr);
void  dump_free_lists(void);

