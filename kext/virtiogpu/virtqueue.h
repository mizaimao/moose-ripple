/* Split virtqueue (virtio 1.0, section 2.4), plain C so it can use Linux's virtio_ring.h. */
#ifndef RIPPLE_VIRTQUEUE_H
#define RIPPLE_VIRTQUEUE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ripple_vq {
    unsigned  num;            /* ring size, a power of two */
    void     *desc;           /* struct vring_desc[num] */
    void     *avail;          /* struct vring_avail */
    void     *used;           /* struct vring_used */
    uint64_t  desc_pa, avail_pa, used_pa;
    uint16_t  free_head;      /* first free descriptor (free list through .next) */
    uint16_t  num_free;
    uint16_t  last_used;      /* next used-ring slot we haven't consumed */
};

/* Bytes needed for a ring of `num` entries laid out with the legacy alignment (4096). */
unsigned ripple_vq_bytes(unsigned num);
/* Lay the ring out in `mem` (zeroed, physically contiguous at `pa`, page aligned). */
void ripple_vq_init(struct ripple_vq *vq, unsigned num, void *mem, uint64_t pa);
/* Add one buffer chain: `nout` device-readable then `nin` device-writable segments.
 * Returns the head descriptor index, or -1 if there aren't enough free descriptors. */
int ripple_vq_add(struct ripple_vq *vq, const uint64_t *pa, const uint32_t *len,
                  unsigned nout, unsigned nin);
/* Ask the device to interrupt (1) or not (0) when it uses buffers. */
void ripple_vq_set_interrupts(struct ripple_vq *vq, int enable);
/* Take one completed chain off the used ring and free its descriptors.
 * Returns 1 and sets *head and *written if there was one, else 0. */
int ripple_vq_get_used(struct ripple_vq *vq, uint16_t *head, uint32_t *written);

#ifdef __cplusplus
}
#endif
#endif
