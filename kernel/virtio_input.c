// Minimal virtio-input PCI tablet/mouse driver.
//
// QEMU's virtio-tablet-pci provides absolute pointer events without relying on
// GTK pointer grabs or the VMware backdoor.  This driver consumes the event
// queue and forwards translated events into the existing /dev/mouse ring.

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "errno.h"
#include "printf.h"
#include "string.h"
#include "trap.h"
#include "dev/pci.h"
#include "dev/virtio.h"
#include "dev/ps2mouse.h"
#include "lock/spinlock.h"
#include <proc/thread.h>
#include "arch/vm.h"
#include <mm/pgtable.h>
#include <mm/vm.h>

extern void sleep_ms(uint64 ms);

#if defined(__x86_64__) || defined(__i386__)

#define VIRTIO_INPUT_MAX_EVENTS 64

extern pagetable_t kernel_pagetable;

#define VIRTIO_INPUT_CFG_ID_NAME  0x01
#define VIRTIO_INPUT_CFG_ABS_INFO 0x03

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define REL_X     0x00
#define REL_Y     0x01
#define REL_WHEEL 0x08

#define ABS_X 0x00
#define ABS_Y 0x01

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct virtio_input_absinfo {
    uint32 min;
    uint32 max;
    uint32 fuzz;
    uint32 flat;
    uint32 res;
} __attribute__((packed));

struct virtio_input_config {
    uint8 select;
    uint8 subsel;
    uint8 size;
    uint8 reserved[5];
    union {
        char string[128];
        uint8 bitmap[128];
        struct virtio_input_absinfo abs;
    } u;
} __attribute__((packed));

struct virtio_input_event {
    uint16 type;
    uint16 code;
    int32 value;
} __attribute__((packed));

struct virtio_input_queue {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    struct virtio_input_event *events;
    uint16 size;
    uint16 used_idx;
    uint16 notify_off;
    spinlock_t lock;
};

struct virtio_input {
    int initialized;
    struct virtio_pci_state pci;
    volatile struct virtio_input_config *config;
    struct virtio_input_queue eventq;
    uint32 abs_x_min;
    uint32 abs_x_max;
    uint32 abs_y_min;
    uint32 abs_y_max;
    uint16 abs_x;
    uint16 abs_y;
    int have_abs;
    int rel_x;
    int rel_y;
    int wheel;
    uint8 buttons;
    int pending;
    uint64 events_seen;
    uint64 events_pushed;
};

static struct virtio_input input;

static uint64 virtio_input_bar_base(struct virtio_pci_discovery *vd, uint8 bar)
{
    uint64 base = (uint64)(vd->bar[bar] & ~0xFU);
    if ((vd->bar[bar] & 0x6) == 0x4 && bar < 5)
        base |= ((uint64)vd->bar[bar + 1]) << 32;
    return base;
}

static uint64 virtio_input_map_mmio_window(uint64 bar, uint32 offset,
                                           uint32 length)
{
    if (bar & 0x1)
        panic("virtio_input_pci: capability uses I/O BAR 0x%lx", bar);

    uint64 target = bar + offset;
    uint64 start = PGROUNDDOWN(target);
    uint64 end = PGROUNDUP(target + (length ? length : 1));
    uint64 size = end - start;
    uint64 map_base;

    vm_wlock(kernel_vm);
    map_base = vm_find_free_range(kernel_vm, size, 0);
    if (map_base == 0) {
        vm_wunlock(kernel_vm);
        panic("virtio_input_pci: failed to allocate MMIO VA window");
    }

    vma_t *vma = vma_alloc(kernel_vm, map_base, size,
                           PROT_READ | PROT_WRITE | VMA_FLAG_KERNEL);
    vm_wunlock(kernel_vm);
    if (vma == NULL)
        panic("virtio_input_pci: failed to reserve MMIO VA window");

    for (uint64 page_off = 0; page_off < size; page_off += PGSIZE) {
        uint64 va = map_base + page_off;
        uint64 pa = start + page_off;

        if (arch_vm_map(kernel_pagetable, va, PGSIZE, pa,
                        PTE_R | PTE_W) != 0)
            panic("virtio_input_pci: failed to map MMIO page pa=0x%lx", pa);
    }

    arch_tlb_flush();
    return map_base + (target - start);
}

static void virtio_input_notify(struct virtio_input *in, uint16 queue)
{
    struct virtio_input_queue *q = &in->eventq;
    volatile uint16 *notify_addr = (volatile uint16 *)
        ((uint8 *)in->pci.notify_base +
         q->notify_off * in->pci.notify_off_multiplier);
    *notify_addr = queue;
}

static uint16 virtio_input_scale_abs(int32 value, uint32 min, uint32 max)
{
    if (max <= min)
        return (uint16)value;
    if (value < (int32)min)
        value = (int32)min;
    if (value > (int32)max)
        value = (int32)max;
    uint64 span = (uint64)(max - min);
    uint64 pos = (uint64)((uint32)value - min);
    return (uint16)((pos * 65535u) / span);
}

static void virtio_input_emit(struct virtio_input *in)
{
    struct mouse_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.buttons = in->buttons;
    ev.dz = (int8)in->wheel;
    if (in->have_abs) {
        ev.flags = MOUSE_EVENT_F_ABSOLUTE;
        ev.dx = (int16)in->abs_x;
        ev.dy = (int16)in->abs_y;
    } else {
        ev.dx = (int16)in->rel_x;
        ev.dy = (int16)in->rel_y;
    }
    mouse_input_push_event(&ev);
    in->events_pushed++;
}

static void virtio_input_handle_event(struct virtio_input *in,
                                      const struct virtio_input_event *ev)
{
    in->events_seen++;

    switch (ev->type) {
    case EV_ABS:
        if (ev->code == ABS_X) {
            in->abs_x = virtio_input_scale_abs(ev->value, in->abs_x_min,
                                               in->abs_x_max);
            in->have_abs = 1;
            in->pending = 1;
        } else if (ev->code == ABS_Y) {
            in->abs_y = virtio_input_scale_abs(ev->value, in->abs_y_min,
                                               in->abs_y_max);
            in->have_abs = 1;
            in->pending = 1;
        }
        break;
    case EV_REL:
        if (ev->code == REL_X) {
            in->rel_x += ev->value;
            in->pending = 1;
        } else if (ev->code == REL_Y) {
            in->rel_y += ev->value;
            in->pending = 1;
        } else if (ev->code == REL_WHEEL) {
            in->wheel += ev->value;
            in->pending = 1;
        }
        break;
    case EV_KEY:
        if (ev->code == BTN_LEFT) {
            if (ev->value) in->buttons |= 0x01;
            else in->buttons &= ~0x01;
            in->pending = 1;
        } else if (ev->code == BTN_RIGHT) {
            if (ev->value) in->buttons |= 0x02;
            else in->buttons &= ~0x02;
            in->pending = 1;
        } else if (ev->code == BTN_MIDDLE) {
            if (ev->value) in->buttons |= 0x04;
            else in->buttons &= ~0x04;
            in->pending = 1;
        }
        break;
    case EV_SYN:
        if (in->pending || in->wheel) {
            virtio_input_emit(in);
            in->rel_x = 0;
            in->rel_y = 0;
            in->wheel = 0;
            in->pending = 0;
        }
        break;
    default:
        break;
    }
}

static void virtio_input_repost(struct virtio_input *in, uint16 id)
{
    struct virtio_input_queue *q = &in->eventq;

    q->desc[id].addr = (uint64)&q->events[id];
    q->desc[id].len = sizeof(q->events[id]);
    q->desc[id].flags = VRING_DESC_F_WRITE;
    q->desc[id].next = 0;
    q->avail->ring[q->avail->idx % q->size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void virtio_input_drain(struct virtio_input *in)
{
    struct virtio_input_queue *q = &in->eventq;

    spin_lock(&q->lock);
    while (q->used_idx != q->used->idx) {
        uint16 used_slot = q->used_idx % q->size;
        uint32 id = q->used->ring[used_slot].id;
        q->used_idx++;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (id < q->size) {
            struct virtio_input_event ev = q->events[id];
            virtio_input_repost(in, (uint16)id);
            spin_unlock(&q->lock);
            virtio_input_handle_event(in, &ev);
            spin_lock(&q->lock);
        }
    }
    spin_unlock(&q->lock);
    virtio_input_notify(in, 0);
}

static void virtio_input_intr(int irq, void *data, device_t *dev)
{
    struct virtio_input *in = (struct virtio_input *)data;

    (void)irq;
    (void)dev;
    if (in == NULL || !in->initialized)
        return;
    if (in->pci.isr != NULL) {
        volatile uint8 isr_status = *in->pci.isr;
        (void)isr_status;
    }
    virtio_input_drain(in);
}

static void virtio_input_poll_thread(uint64 arg1, uint64 arg2)
{
    struct virtio_input *in = (struct virtio_input *)arg1;
    (void)arg2;
    for (;;) {
        if (in->initialized)
            virtio_input_drain(in);
        sleep_ms(8);
    }
}

static int virtio_input_queue_init(struct virtio_input *in)
{
    volatile struct virtio_pci_common_cfg *cfg = in->pci.common_cfg;
    struct virtio_input_queue *q = &in->eventq;

    cfg->queue_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16 max = cfg->queue_size;
    if (max == 0) {
        printf("virtio_input: event queue missing\n");
        return -1;
    }

    uint16 qsize = VIRTIO_INPUT_MAX_EVENTS;
    if (max < qsize)
        qsize = max;
    if (qsize > NUM)
        qsize = NUM;

    q->desc = kalloc();
    q->avail = kalloc();
    q->used = kalloc();
    q->events = kalloc();
    if (!q->desc || !q->avail || !q->used || !q->events)
        panic("virtio_input: kalloc");

    memset(q->desc, 0, PGSIZE);
    memset(q->avail, 0, PGSIZE);
    memset(q->used, 0, PGSIZE);
    memset(q->events, 0, PGSIZE);

    cfg->queue_size = qsize;
    cfg->queue_desc = (uint64)q->desc;
    cfg->queue_driver = (uint64)q->avail;
    cfg->queue_device = (uint64)q->used;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    cfg->queue_enable = 1;
    q->size = qsize;
    q->notify_off = cfg->queue_notify_off;
    spin_init(&q->lock, "virtio_inputq");

    for (uint16 i = 0; i < qsize; i++)
        virtio_input_repost(in, i);
    virtio_input_notify(in, 0);
    return 0;
}

static void virtio_input_read_absinfo(struct virtio_input *in, uint8 axis,
                                      uint32 *min, uint32 *max)
{
    volatile struct virtio_input_config *cfg = in->config;

    cfg->select = VIRTIO_INPUT_CFG_ABS_INFO;
    cfg->subsel = axis;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (cfg->size >= sizeof(struct virtio_input_absinfo) && cfg->u.abs.max)
    {
        *min = cfg->u.abs.min;
        *max = cfg->u.abs.max;
    }
}

void virtio_input_init(void)
{
    struct virtio_pci_discovery *vd = pci_get_virtio_input(0);
    if (!vd || !vd->found)
        return;

    if (!vd->common_cfg_cap || !vd->notify_cfg_cap || !vd->isr_cfg_cap ||
        !vd->device_cfg_cap) {
        printf("virtio_input: missing PCI capability, skipping driver init\n");
        return;
    }

    uint8 ccap = vd->common_cfg_cap;
    uint8 ncap = vd->notify_cfg_cap;
    uint8 icap = vd->isr_cfg_cap;
    uint8 dcap = vd->device_cfg_cap;

    uint8 cc_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ccap + 4);
    uint32 cc_off = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 8);
    uint32 cc_len = pci_config_read32(vd->bus, vd->dev, vd->func, ccap + 12);
    uint8 n_bar = pci_config_read8(vd->bus, vd->dev, vd->func, ncap + 4);
    uint32 n_off = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 8);
    uint32 n_len = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 12);
    uint32 n_mult = pci_config_read32(vd->bus, vd->dev, vd->func, ncap + 16);
    uint8 i_bar = pci_config_read8(vd->bus, vd->dev, vd->func, icap + 4);
    uint32 i_off = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 8);
    uint32 i_len = pci_config_read32(vd->bus, vd->dev, vd->func, icap + 12);
    uint8 d_bar = pci_config_read8(vd->bus, vd->dev, vd->func, dcap + 4);
    uint32 d_off = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 8);
    uint32 d_len = pci_config_read32(vd->bus, vd->dev, vd->func, dcap + 12);

    struct virtio_input *in = &input;
    memset(in, 0, sizeof(*in));
    in->abs_x_max = 0x7fff;
    in->abs_y_max = 0x7fff;

    in->pci.use_pci = 1;
    in->pci.common_cfg = (volatile struct virtio_pci_common_cfg *)
        virtio_input_map_mmio_window(virtio_input_bar_base(vd, cc_bar),
                                     cc_off, cc_len);
    in->pci.notify_base = (volatile uint16 *)
        virtio_input_map_mmio_window(virtio_input_bar_base(vd, n_bar),
                                     n_off, n_len);
    in->pci.notify_off_multiplier = n_mult;
    in->pci.isr = (volatile uint8 *)
        virtio_input_map_mmio_window(virtio_input_bar_base(vd, i_bar),
                                     i_off, i_len);
    in->config = (volatile struct virtio_input_config *)
        virtio_input_map_mmio_window(virtio_input_bar_base(vd, d_bar),
                                     d_off, d_len);

    virtio_input_read_absinfo(in, ABS_X, &in->abs_x_min, &in->abs_x_max);
    virtio_input_read_absinfo(in, ABS_Y, &in->abs_y_min, &in->abs_y_max);

    volatile struct virtio_pci_common_cfg *cfg = in->pci.common_cfg;
    cfg->device_status = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (cfg->device_status != 0)
        ;

    uint8 status = VIRTIO_CONFIG_S_ACKNOWLEDGE;
    cfg->device_status = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    cfg->device_status = status;

    cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cfg->driver_feature = 0;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(cfg->device_status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        printf("virtio_input: device rejected feature set\n");
        cfg->device_status = 0;
        return;
    }

    if (virtio_input_queue_init(in) != 0) {
        cfg->device_status = 0;
        return;
    }

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    cfg->device_status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    in->initialized = 1;

    struct irq_desc irq_desc = {
        .handler = virtio_input_intr,
        .data = in,
        .dev = NULL,
    };
    int irq_ret = register_irq_handler(PLIC_IRQ(vd->irq_line), &irq_desc);
    if (irq_ret == 0) {
        extern void plic_enable_irq_level(int irq);
        plic_enable_irq_level(vd->irq_line);
    } else {
        printf("virtio_input: WARNING: IRQ %d registration failed (%d), polling only\n",
               vd->irq_line, irq_ret);
    }

    struct thread *poller = kthread_create("virtio_input_poll",
                                           virtio_input_poll_thread,
                                           (uint64)in, 0, 0);
    if (IS_ERR_OR_NULL(poller))
        printf("virtio_input: failed to start poll thread\n");

    in->config->select = VIRTIO_INPUT_CFG_ID_NAME;
    in->config->subsel = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    printf("virtio_input: initialized queues=%u eventq=%u abs_x=%u..%u abs_y=%u..%u irq=%d name=%s\n",
           cfg->num_queues, in->eventq.size, in->abs_x_min, in->abs_x_max,
           in->abs_y_min, in->abs_y_max, vd->irq_line,
           in->config->size ? in->config->u.string : "");
}

#else

void virtio_input_init(void) {}

#endif
