/* Guest memory for OpenGL resources (gpu_memory.c). No kernel headers needed. */
#ifndef RB2K_GPU_MEMORY_H
#define RB2K_GPU_MEMORY_H

typedef struct {
    PVOID mdl, user, owner;
    ULONG bytes;
} RB2K_PAGES;

/* The layout of struct virtio_gpu_mem_entry. */
typedef struct {
    unsigned __int64 address;
    ULONG length, padding;
} RB2K_PAGE_RUN;

PVOID rb_process_ref(void);
void rb_process_deref(PVOID process);
BOOLEAN rb_process_current(PVOID process);
BOOLEAN rb_process_gone(PVOID process);
LONGLONG rb_counter_ticks(LONGLONG *frequency);
BOOLEAN rb_pages_alloc(RB2K_PAGES *pages, ULONG bytes);
ULONG rb_pages_runs(const RB2K_PAGES *pages, RB2K_PAGE_RUN *out, ULONG max);
void rb_pages_free(RB2K_PAGES *pages);
PVOID rb_system_pages(ULONG bytes, PVOID *mdl);
PVOID rb_map_cached(unsigned __int64 physical, ULONG bytes);
void rb_unmap_cached(PVOID address, ULONG bytes);

#endif
