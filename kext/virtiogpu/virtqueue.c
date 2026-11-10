/* Split virtqueue for the virtio-gpu kext. See virtqueue.h. */
/* Full barrier: x86 keeps stores ordered, but the compiler must not reorder around it. */
#define vq_mb() __asm__ __volatile__("mfence" ::: "memory")
#include <string.h>
#include <linux/virtio_ring.h>
#include "virtqueue.h"

#define VQ_ALIGN 4096

unsigned ripple_vq_bytes(unsigned num)
{
    return vring_size(num, VQ_ALIGN);
}

void ripple_vq_init(struct ripple_vq *vq, unsigned num, void *mem, uint64_t pa)
{
    struct vring vr;
    unsigned i;

    vring_init(&vr, num, mem, VQ_ALIGN);
    vq->num = num;
    vq->desc = vr.desc;
    vq->avail = vr.avail;
    vq->used = vr.used;
    vq->desc_pa = pa;
    vq->avail_pa = pa + ((uintptr_t)vr.avail - (uintptr_t)mem);
    vq->used_pa = pa + ((uintptr_t)vr.used - (uintptr_t)mem);
    for (i = 0; i < num; i++)
        vr.desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->num_free = (uint16_t)num;
    vq->last_used = 0;
}

int ripple_vq_add(struct ripple_vq *vq, const uint64_t *pa, const uint32_t *len,
                  unsigned nout, unsigned nin)
{
    struct vring_desc *desc = vq->desc;
    struct vring_avail *avail = vq->avail;
    unsigned total = nout + nin, i;
    uint16_t head, cur, prev = 0;

    if (total == 0 || total > vq->num_free)
        return -1;
    head = cur = vq->free_head;
    for (i = 0; i < total; i++) {
        desc[cur].addr = pa[i];
        desc[cur].len = len[i];
        desc[cur].flags = (uint16_t)((i < total - 1 ? VRING_DESC_F_NEXT : 0) |
                                     (i >= nout ? VRING_DESC_F_WRITE : 0));
        prev = cur;
        cur = desc[cur].next;
    }
    vq->free_head = cur;
    vq->num_free = (uint16_t)(vq->num_free - total);
    (void)prev;

    avail->ring[avail->idx % vq->num] = head;
    vq_mb();                    /* descriptors and ring entry before the index */
    avail->idx = (uint16_t)(avail->idx + 1);
    vq_mb();
    return head;
}

int ripple_vq_get_used(struct ripple_vq *vq, uint16_t *head, uint32_t *written)
{
    struct vring_used *used = vq->used;
    struct vring_desc *desc = vq->desc;
    uint16_t id, cur;
    unsigned n = 1;

    if (*(volatile uint16_t *)&used->idx == vq->last_used)
        return 0;
    vq_mb();                    /* read the entry only after seeing the index */
    id = (uint16_t)used->ring[vq->last_used % vq->num].id;
    if (written)
        *written = used->ring[vq->last_used % vq->num].len;
    vq->last_used = (uint16_t)(vq->last_used + 1);

    cur = id;
    while (desc[cur].flags & VRING_DESC_F_NEXT) {
        cur = desc[cur].next;
        n++;
    }
    desc[cur].next = vq->free_head;
    vq->free_head = id;
    vq->num_free = (uint16_t)(vq->num_free + n);
    if (head)
        *head = id;
    return 1;
}
