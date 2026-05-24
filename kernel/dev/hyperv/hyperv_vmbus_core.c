static void hvdxg_ring_init(uint8 *ring, struct hv_ring_buffer **out_ring,
                            struct hv_ring_buffer **in_ring)
{
    *out_ring = (struct hv_ring_buffer *)ring;
    *in_ring = (struct hv_ring_buffer *)(ring + HV_SEND_PAGES * PGSIZE);
    memset(ring, 0, HV_RING_PAGES * PGSIZE);
    (*out_ring)->feature_bits = 1;
    (*in_ring)->feature_bits = 1;
}

static spinlock_t hvvideo_dirty_lock =
    SPINLOCK_INITIALIZED("hyperv_video_dirty");

static uint32 hv_unknown_offer_count;
static volatile int hv_gpadl_wait_ok;
static volatile uint32 hv_gpadl_wait_handle;
static uint32 hv_gpadl_wait_status;
static int hv_debug_cached = -1;

static int hv_cmdline_enabled(const char *key)
{
    char buf[16];

    return cmdline_get_param(key, buf, sizeof(buf)) == 0 &&
           (strcmp(buf, "1") == 0 ||
            strcmp(buf, "yes") == 0 ||
            strcmp(buf, "true") == 0 ||
            strcmp(buf, "on") == 0);
}

static int hv_debug_enabled(void)
{
    if (hv_debug_cached >= 0)
        return hv_debug_cached;
    hv_debug_cached = hv_cmdline_enabled("hyperv_debug");
    return hv_debug_cached;
}

static void hv_cpuid(uint32 leaf, uint32 *a, uint32 *b, uint32 *c, uint32 *d)
{
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(0));
}

static int hv_is_hyperv(void)
{
    uint32 a, b, c, d;
    hv_cpuid(1, &a, &b, &c, &d);
    if (!(c & (1U << 31)))
        return 0;
    hv_cpuid(0x40000000, &a, &b, &c, &d);
    return b == 0x7263694d && c == 0x666f736f && d == 0x76482074;
}

static uint64 hv_do_hypercall(uint64 control, uint64 input, uint64 output)
{
    uint64 ret;
    register uint64 r8 asm("r8") = output;
    void *page = hv.hypercall_page;

    asm volatile("call *%4"
                 : "=a"(ret)
                 : "c"(control), "d"(input), "r"(r8), "r"(page)
                 : "r9", "r10", "r11", "memory", "cc");
    return ret;
}

static uint64 hv_do_fast_hypercall8(uint64 control, uint64 input)
{
    uint64 ret;
    void *page = hv.hypercall_page;

    asm volatile("call *%3"
                 : "=a"(ret)
                 : "c"(control | HV_HYPERCALL_FAST), "d"(input),
                   "r"(page)
                 : "r8", "r9", "r10", "r11", "memory", "cc");
    return ret;
}

static int hv_alloc_page(uint64 *pa_out, void **va_out)
{
    void *pa = page_alloc(0, PAGE_TYPE_ANON);
    if (pa == NULL)
        return -ENOMEM;
    memset(pa, 0, PGSIZE);
    *pa_out = (uint64)pa;
    *va_out = pa;
    return 0;
}

static int hv_post_msg(uint32 msg_conn_id, const void *payload, uint32 size)
{
    if (size > 240)
        return -EINVAL;

    struct hv_input_post_message *msg =
        (struct hv_input_post_message *)hv.post_page;
    memset(msg, 0, sizeof(*msg));
    msg->connection_id = msg_conn_id;
    msg->message_type = HVMSG_CHANNEL;
    msg->payload_size = size;
    memcpy(msg->payload, payload, size);

    for (int i = 0; i < 100; i++) {
        uint64 status =
            hv_do_hypercall(HVCALL_POST_MESSAGE, hv.post_pa, 0) & 0xffff;
        if (status == HV_STATUS_SUCCESS)
            return 0;
        if (status != HV_STATUS_INSUFFICIENT_MEMORY &&
            status != HV_STATUS_INSUFFICIENT_BUFFERS &&
            status != HV_STATUS_INVALID_CONNECTION_ID) {
            printf("hyperv-vmbus: post msg type=%u size=%u failed status=%lu\n",
                   ((const struct vmbus_msg_hdr *)payload)->msgtype, size,
                   status);
            return -EIO;
        }
        sleep_ms(i < 8 ? 1 : 2);
    }
    printf("hyperv-vmbus: post msg type=%u size=%u exhausted retries\n",
           ((const struct vmbus_msg_hdr *)payload)->msgtype, size);
    return -EAGAIN;
}

static void hv_set_monitor_event(int monitor_allocated, uint8 monitorid)
{
    if (!monitor_allocated || hv.monitor2 == NULL)
        return;

    struct hv_monitor_page *page = (struct hv_monitor_page *)hv.monitor2;
    uint32 group = monitorid / 32;
    uint32 bit = monitorid % 32;
    if (group >= 4)
        return;

    uint32 mask = 1U << bit;
    __atomic_fetch_or(&page->trigger_group[group].pending, mask,
                      __ATOMIC_RELEASE);
}

static void hv_send_interrupt(uint32 child_relid)
{
    if (hv.send_int_page == NULL || child_relid >= HV_EVENT_FLAGS_BYTES * 8)
        return;

    volatile uint64 *flags = (volatile uint64 *)hv.send_int_page;
    uint32 word = child_relid / 64;
    uint64 mask = 1ULL << (child_relid % 64);
    __atomic_fetch_or(&flags[word], mask, __ATOMIC_RELEASE);
}

static void hv_clear_channel_signal(uint32 child_relid, int monitor_allocated,
                                    uint8 monitorid)
{
    if (hv.send_int_page != NULL && child_relid < HV_EVENT_FLAGS_BYTES * 8) {
        volatile uint64 *flags = (volatile uint64 *)hv.send_int_page;
        uint32 word = child_relid / 64;
        uint64 mask = 1ULL << (child_relid % 64);
        __atomic_fetch_and(&flags[word], ~mask, __ATOMIC_ACQ_REL);
    }
    if (monitor_allocated && hv.monitor2 != NULL) {
        struct hv_monitor_page *page = (struct hv_monitor_page *)hv.monitor2;
        uint32 group = monitorid / 32;
        uint32 bit = monitorid % 32;
        if (group < 4)
            __atomic_fetch_and(&page->trigger_group[group].pending,
                               ~(1U << bit), __ATOMIC_ACQ_REL);
    }
}

static void hv_signal_channel(uint32 child_relid, uint32 signal_conn_id,
                              int monitor_allocated, uint8 monitorid,
                              int dedicated)
{
    static uint32 debug_signal_count;

    if (monitor_allocated) {
        hv_send_interrupt(child_relid);
        hv_set_monitor_event(monitor_allocated, monitorid);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (hv_debug_enabled() && debug_signal_count < 8) {
            volatile uint64 *bits = (volatile uint64 *)hv.send_int_page;
            struct hv_monitor_page *page =
                (struct hv_monitor_page *)hv.monitor2;
            uint32 group = monitorid / 32;
            uint32 pending = (page && group < 4) ?
                page->trigger_group[group].pending : 0;
            printf("hyperv-vmbus: signal relid=%u conn=%u monitor=%d monid=%u intword=0x%lx pending=0x%x\n",
                   child_relid, signal_conn_id, monitor_allocated, monitorid,
                   bits ? bits[child_relid / 64] : 0, pending);
            debug_signal_count++;
        }
        return;
    }
    if (!dedicated)
        hv_send_interrupt(child_relid);
    if (signal_conn_id != 0)
        hv_do_fast_hypercall8(HVCALL_SIGNAL_EVENT, signal_conn_id);
}

static int hv_ring_should_signal(struct hv_ring_buffer *out_ring,
                                 uint32 old_write)
{
    if (__atomic_load_n(&out_ring->interrupt_mask, __ATOMIC_ACQUIRE) != 0)
        return 0;

    uint32 read = __atomic_load_n(&out_ring->read_index, __ATOMIC_ACQUIRE);
    return read == old_write;
}

static int guid_eq(const struct hv_guid *a, const struct hv_guid *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void hv_print_guid(const struct hv_guid *g)
{
    printf("%lx-%lx-%lx-%lx%lx-%lx%lx%lx%lx%lx%lx",
           (uint64)g->a, (uint64)g->b, (uint64)g->c,
           (uint64)g->d[0], (uint64)g->d[1], (uint64)g->d[2],
           (uint64)g->d[3], (uint64)g->d[4], (uint64)g->d[5],
           (uint64)g->d[6], (uint64)g->d[7]);
}

static void hv_log_offer(const char *name, const struct vmbus_offer_channel *o)
{
    printf("hyperv-vmbus: %s offer relid=%u conn=%u monitor=%u allocated=%d dedicated=%d subidx=%u type=",
           name, o->child_relid, o->connection_id, o->monitorid,
           o->monitor_allocated != 0, o->dedicated != 0,
           o->offer.sub_channel_index);
    hv_print_guid(&o->offer.if_type);
    printf(" instance=");
    hv_print_guid(&o->offer.if_instance);
    printf("\n");
}

static void hv_eom(struct hv_message *msg, uint32 old_type)
{
    __atomic_store_n(&msg->header.message_type, HVMSG_NONE, __ATOMIC_RELEASE);
    if (msg->header.message_flags & 1)
        wrmsr(HV_MSR_EOM, 0);
    (void)old_type;
}

static void hv_handle_channel_msg(const void *payload)
{
    const struct vmbus_msg_hdr *hdr = (const struct vmbus_msg_hdr *)payload;

    switch (hdr->msgtype) {
    case CHANNELMSG_VERSION_RESPONSE: {
        const struct vmbus_version_response *r = payload;
        if (r->supported) {
            hv.connected = 1;
            if (r->msg_conn_id)
                hv.msg_conn_id = r->msg_conn_id;
        }
        break;
    }
    case CHANNELMSG_OFFERCHANNEL: {
        const struct vmbus_offer_channel *offer = payload;
        if (offer->offer.sub_channel_index == 0 &&
            guid_eq(&offer->offer.if_type, &hv_mouse_guid)) {
            hv.child_relid = offer->child_relid;
            hv.signal_conn_id = offer->connection_id;
            hv.monitorid = offer->monitorid;
            hv.monitor_allocated = offer->monitor_allocated != 0;
            hv.dedicated = offer->dedicated != 0;
            hv_log_offer("synthhid-mouse", offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   guid_eq(&offer->offer.if_type, &hv_kbd_guid)) {
            hvkbd.present = 1;
            hvkbd.child_relid = offer->child_relid;
            hvkbd.signal_conn_id = offer->connection_id;
            hvkbd.monitorid = offer->monitorid;
            hvkbd.monitor_allocated = offer->monitor_allocated != 0;
            hvkbd.dedicated = offer->dedicated != 0;
            hv_log_offer("synthkbd", offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   (guid_eq(&offer->offer.if_type, &hv_scsi_guid) ||
                    guid_eq(&offer->offer.if_type, &hv_ide_guid))) {
            hvstor.present = 1;
            hvstor.is_ide = guid_eq(&offer->offer.if_type, &hv_ide_guid);
            hvstor.child_relid = offer->child_relid;
            hvstor.signal_conn_id = offer->connection_id;
            hvstor.monitorid = offer->monitorid;
            hvstor.monitor_allocated = offer->monitor_allocated != 0;
            hvstor.dedicated = offer->dedicated != 0;
            hv_log_offer(hvstor.is_ide ? "storvsc-ide" : "storvsc-scsi",
                         offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   guid_eq(&offer->offer.if_type, &hv_net_guid)) {
            hvnet.present = 1;
            hvnet.child_relid = offer->child_relid;
            hvnet.signal_conn_id = offer->connection_id;
            hvnet.monitorid = offer->monitorid;
            hvnet.monitor_allocated = offer->monitor_allocated != 0;
            hvnet.dedicated = offer->dedicated != 0;
            hv_log_offer("netvsc", offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   guid_eq(&offer->offer.if_type, &hv_video_guid)) {
            hvvideo.present = 1;
            hvvideo.child_relid = offer->child_relid;
            hvvideo.signal_conn_id = offer->connection_id;
            hvvideo.monitorid = offer->monitorid;
            hvvideo.monitor_allocated = offer->monitor_allocated != 0;
            hvvideo.dedicated = offer->dedicated != 0;
            hv_log_offer("synthetic-video", offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   guid_eq(&offer->offer.if_type, &hv_dxg_global_guid)) {
            hvdxg.global_present = 1;
            hvdxg.global_relid = offer->child_relid;
            hvdxg.global_conn_id = offer->connection_id;
            hvdxg.global_monitorid = offer->monitorid;
            hvdxg.global_monitor_allocated = offer->monitor_allocated != 0;
            hvdxg.global_dedicated = offer->dedicated != 0;
            hvdxg.global_mmio_megabytes = offer->offer.mmio_megabytes;
            memcpy(hvdxg.global_offer_user_def, offer->offer.user_def,
                   sizeof(hvdxg.global_offer_user_def));
            hvdxg.global_instance = offer->offer.if_instance;
            hv_log_offer("gpu-pv-dxg-global", offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   guid_eq(&offer->offer.if_type, &hv_dxg_vgpu_guid)) {
            hvdxg.vgpu_present = 1;
            hvdxg.vgpu_count++;
            if (hvdxg.vgpu_count == 1) {
                hvdxg.vgpu_relid = offer->child_relid;
                hvdxg.vgpu_conn_id = offer->connection_id;
                hvdxg.vgpu_monitorid = offer->monitorid;
                hvdxg.vgpu_monitor_allocated = offer->monitor_allocated != 0;
                hvdxg.vgpu_dedicated = offer->dedicated != 0;
                memcpy(hvdxg.vgpu_offer_user_def, offer->offer.user_def,
                       sizeof(hvdxg.vgpu_offer_user_def));
                hvdxg.vgpu_instance = offer->offer.if_instance;
            }
            hv_log_offer("gpu-pv-dxg-vgpu", offer);
        } else if (guid_eq(&offer->offer.if_type, &hv_pci_guid)) {
            hvpci.offer_count++;
            hvdxg.hyperv_pci_offer_count++;
            if (!hvpci.present) {
                hvpci.present = 1;
                hvpci.child_relid = offer->child_relid;
                hvpci.signal_conn_id = offer->connection_id;
                hvpci.monitorid = offer->monitorid;
                hvpci.monitor_allocated = offer->monitor_allocated != 0;
                hvpci.dedicated = offer->dedicated != 0;
                hvpci.offer_flags = offer->offer.flags;
                hvpci.offer_mmio_megabytes = offer->offer.mmio_megabytes;
                memcpy(hvpci.offer_user_def, offer->offer.user_def,
                       sizeof(hvpci.offer_user_def));
                hvpci.offer_instance = offer->offer.if_instance;

                hvdxg.hyperv_pci_offer_present = 1;
                hvdxg.hyperv_pci_offer_relid = offer->child_relid;
                hvdxg.hyperv_pci_offer_conn_id = offer->connection_id;
                hvdxg.hyperv_pci_offer_monitorid = offer->monitorid;
                hvdxg.hyperv_pci_offer_monitor_allocated =
                    offer->monitor_allocated != 0;
                hvdxg.hyperv_pci_offer_dedicated = offer->dedicated != 0;
                hvdxg.hyperv_pci_offer_flags = offer->offer.flags;
                hvdxg.hyperv_pci_offer_mmio_megabytes =
                    offer->offer.mmio_megabytes;
                memcpy(hvdxg.hyperv_pci_offer_user_def,
                       offer->offer.user_def,
                       sizeof(hvdxg.hyperv_pci_offer_user_def));
                hvdxg.hyperv_pci_offer_instance = offer->offer.if_instance;
            }
            hv_log_offer("hyperv-pci", offer);
        } else if (offer->offer.sub_channel_index == 0 &&
                   hv_unknown_offer_count < 32) {
            hv_unknown_offer_count++;
            hv_log_offer("unknown", offer);
        }
        break;
    }
    case CHANNELMSG_ALLOFFERS_DELIVERED:
        hv.all_offers = 1;
        break;
    case CHANNELMSG_GPADL_CREATED: {
        const struct vmbus_gpadl_created *g = payload;
        if (g->gpadl == hv_gpadl_wait_handle) {
            hv_gpadl_wait_status = g->status;
            hv_gpadl_wait_ok = (g->status == 0);
        }
        if (g->child_relid == hv.child_relid && g->gpadl == HV_GPADL_HANDLE) {
            hv.gpadl_status = g->status;
            hv.gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvkbd.child_relid &&
                   g->gpadl == HV_KBD_GPADL_HANDLE) {
            hvkbd.gpadl_status = g->status;
            hvkbd.gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvstor.child_relid &&
                   g->gpadl == HV_STOR_GPADL_HANDLE) {
            hvstor.gpadl_status = g->status;
            hvstor.gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvnet.child_relid &&
                   g->gpadl == HV_NET_GPADL_HANDLE) {
            hvnet.gpadl_status = g->status;
            hvnet.gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvnet.child_relid &&
                   g->gpadl == HV_NET_RECV_GPADL_HANDLE) {
            hvnet.recv_gpadl_status = g->status;
            hvnet.recv_gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvnet.child_relid &&
                   g->gpadl == HV_NET_SEND_GPADL_HANDLE) {
            hvnet.send_gpadl_status = g->status;
            hvnet.send_gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvvideo.child_relid &&
                   g->gpadl == HV_VIDEO_GPADL_HANDLE) {
            hvvideo.gpadl_status = g->status;
            hvvideo.gpadl_ok = (g->status == 0);
        } else if (g->child_relid == hvpci.child_relid &&
                   g->gpadl == HV_PCI_GPADL_HANDLE) {
            hvpci.gpadl_status = g->status;
            hvpci.gpadl_ok = (g->status == 0);
        }
        break;
    }
    case CHANNELMSG_OPENCHANNEL_RESULT: {
        const struct vmbus_open_result *r = payload;
        if (r->child_relid == hv.child_relid) {
            hv.open_status = r->status;
            hv.open_ok = (r->status == 0);
        } else if (r->child_relid == hvkbd.child_relid) {
            hvkbd.open_status = r->status;
            hvkbd.open_ok = (r->status == 0);
        } else if (r->child_relid == hvstor.child_relid) {
            hvstor.open_status = r->status;
            hvstor.open_ok = (r->status == 0);
        } else if (r->child_relid == hvnet.child_relid) {
            hvnet.open_status = r->status;
            hvnet.open_ok = (r->status == 0);
        } else if (r->child_relid == hvvideo.child_relid) {
            hvvideo.open_status = r->status;
            hvvideo.open_ok = (r->status == 0);
        } else if (r->child_relid == hvpci.child_relid) {
            hvpci.open_status = r->status;
            hvpci.open_ok = (r->status == 0);
        } else if (r->child_relid == hvdxg.global_relid) {
            hvdxg.global_open_status = r->status;
            hvdxg.global_open_ok = (r->status == 0);
        } else if (r->child_relid == hvdxg.vgpu_relid) {
            hvdxg.vgpu_open_status = r->status;
            hvdxg.vgpu_open_ok = (r->status == 0);
        }
        break;
    }
    default:
        break;
    }
}

static void hv_process_messages(void)
{
    if (hv.msg_page == NULL)
        return;

    struct hv_message *msg = &hv.msg_page[HV_MESSAGE_SINT];
    uint32 type = __atomic_load_n(&msg->header.message_type, __ATOMIC_ACQUIRE);
    if (type == HVMSG_NONE)
        return;

    hv_handle_channel_msg(msg->payload);
    hv_eom(msg, type);
}

static void hv_process_channel_packets(void);
static void hvkbd_process_channel_packets(void);
static void hvstor_process_channel_packets(void);
static void hvnet_process_channel_packets(void);
static void hvpci_process_channel_packets(void);
static void hv_process_events(void);
static int hv_recv_raw_on(struct hv_ring_buffer *in_ring, void *buf,
                          uint32 buflen, uint32 *out_len, uint16 *out_type);
static int hv_send_packet_on(struct hv_ring_buffer *out_ring,
                             uint32 child_relid, uint32 signal_conn_id,
                             int monitor_allocated, uint8 monitorid,
                             int dedicated, const void *payload,
                             uint32 payload_len, uint64 trans_id,
                             uint32 flags);

static int hv_test_and_clear_event(uint32 relid)
{
    if (hv.event_page == NULL || relid >= HV_EVENT_FLAGS_BYTES * 8)
        return 0;

    volatile uint64 *flags = (volatile uint64 *)
        ((uint8 *)hv.event_page + HV_MESSAGE_SINT * HV_EVENT_FLAGS_BYTES);
    uint32 word = relid / 64;
    uint64 mask = 1ULL << (relid % 64);
    uint64 old = __atomic_fetch_and(&flags[word], ~mask, __ATOMIC_ACQ_REL);
    return (old & mask) != 0;
}

static void hvvideo_process_channel_packets(void);

static uint64 hvdxg_next_trans_id(void)
{
    uint64 id = __atomic_add_fetch(&hvdxg.next_trans_id, 1,
                                   __ATOMIC_RELAXED);
    return id == 0 ? __atomic_add_fetch(&hvdxg.next_trans_id, 1,
                                        __ATOMIC_RELAXED) : id;
}

static void hvdxg_command_vgpu_init(struct hvdxg_command_vgpu_to_host *hdr,
                                    uint32 command_type)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->channel_type = HV_DXGKVMB_VGPU_TO_HOST;
    hdr->command_type = command_type;
}

static void hvdxg_command_vgpu_init_process(
    struct hvdxg_command_vgpu_to_host *hdr, uint32 command_type,
    struct hvdxg_d3dkmthandle process)
{
    hvdxg_command_vgpu_init(hdr, command_type);
    hdr->process = process;
}

static inline void hvdxg_wc_store_fence(void)
{
#if defined(__x86_64__)
    __asm__ volatile("mfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static int hvdxg_sync_acquire(void)
{
    for (uint32 i = 0; i < HV_DXG_WAIT_MS; i++) {
        int expected = 0;

        if (__atomic_compare_exchange_n(&hvdxg.sync_active, &expected, 1,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return 0;
        __atomic_add_fetch(&hvdxg.sync_waits, 1, __ATOMIC_RELAXED);
        sleep_ms(1);
    }
    __atomic_add_fetch(&hvdxg.sync_timeouts, 1, __ATOMIC_RELAXED);
    return -ETIMEDOUT;
}

static void hvdxg_sync_release(void)
{
    __atomic_store_n(&hvdxg.sync_active, 0, __ATOMIC_RELEASE);
}

static int hvdxg_send_packet_with_retry(struct hv_ring_buffer *out_ring,
                                        uint32 child_relid,
                                        uint32 signal_conn_id,
                                        int monitor_allocated,
                                        uint8 monitorid, int dedicated,
                                        const void *payload,
                                        uint32 payload_len,
                                        uint64 trans_id, uint32 flags,
                                        uint32 *retry_count,
                                        int32 *last_ret)
{
    uint32 retries = 0;
    int ret;

    for (;;) {
        ret = hv_send_packet_on(out_ring, child_relid, signal_conn_id,
                                monitor_allocated, monitorid, dedicated,
                                payload, payload_len, trans_id, flags);
        if (ret != -EAGAIN || retries >= HV_DXG_SEND_EAGAIN_RETRIES)
            break;
        retries++;
        sleep_ms(1);
    }
    if (retry_count != NULL)
        *retry_count = retries;
    if (last_ret != NULL)
        *last_ret = ret;
    return ret;
}

static void hvdxg_command_vm_init(struct hvdxg_command_vm_to_host *hdr,
                                  uint32 command_type)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->channel_type = HV_DXGKVMB_VM_TO_HOST;
    hdr->command_type = command_type;
}

static int hvdxg_luid_equal(struct hvdxg_winluid a, struct hvdxg_winluid b)
{
    return a.a == b.a && a.b == b.b;
}

static int hvdxg_luid_nonzero(struct hvdxg_winluid luid)
{
    return luid.a != 0 || luid.b != 0;
}

static struct hvdxg_winluid hvdxg_user_adapter_luid(uint32 *source_out)
{
    struct hvdxg_winluid luid;

    if (source_out != NULL)
        *source_out = HV_DXG_USER_LUID_SOURCE_NONE;
    if (hvdxg_luid_nonzero(hvdxg.adapter_luid) &&
        (!hvdxg_luid_nonzero(hvdxg.host_adapter_luid) ||
         !hvdxg_luid_equal(hvdxg.adapter_luid,
                           hvdxg.host_adapter_luid))) {
        if (source_out != NULL)
            *source_out = HV_DXG_USER_LUID_SOURCE_ADAPTER;
        return hvdxg.adapter_luid;
    }
    if (hvdxg_luid_nonzero(hvdxg.host_vgpu_luid)) {
        if (source_out != NULL)
            *source_out = HV_DXG_USER_LUID_SOURCE_HOST_VGPU;
        return hvdxg.host_vgpu_luid;
    }
    if (hvdxg_luid_nonzero(hvdxg.pci_host_vgpu_luid)) {
        if (source_out != NULL)
            *source_out = HV_DXG_USER_LUID_SOURCE_PCI_HOST_VGPU;
        return hvdxg.pci_host_vgpu_luid;
    }
    luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);
    if (hvdxg_luid_nonzero(luid)) {
        if (source_out != NULL)
            *source_out = HV_DXG_USER_LUID_SOURCE_GUID;
        return luid;
    }
    return luid;
}

static struct hvdxg_winluid hvdxg_ext_adapter_luid(
    const struct hvdxg_winluid *process_luid)
{
    struct hvdxg_winluid zero;

    if (process_luid != NULL && hvdxg_luid_nonzero(*process_luid))
        return *process_luid;
    if (hvdxg_luid_nonzero(hvdxg.host_vgpu_luid))
        return hvdxg.host_vgpu_luid;
    if (hvdxg_luid_nonzero(hvdxg.pci_host_vgpu_luid))
        return hvdxg.pci_host_vgpu_luid;
    memset(&zero, 0, sizeof(zero));
    return zero;
}

static int hvdxg_ntstatus_plausible(struct hvdxg_ntstatus status)
{
    uint32 v = (uint32)status.v;

    switch (v) {
    case 0x00000000U:
    case HV_DXG_STATUS_PENDING:
    case 0x80000005U:
    case 0x80000006U:
    case 0xC0000001U:
    case 0xC0000008U:
    case 0xC000000DU:
    case 0xC0000017U:
    case 0xC0000022U:
    case 0xC0000023U:
    case 0xC00002B6U:
        return 1;
    default:
        break;
    }

    return (v & 0xC0000000U) == 0xC0000000U;
}

static int hvdxg_ntstatus_to_errno(struct hvdxg_ntstatus status)
{
    if (status.v >= 0)
        return status.v;
    switch ((uint32)status.v) {
    case 0xC0000008U:
        return -EBADF;
    case 0xC000000DU:
        return -EINVAL;
    case 0xC0000017U:
        return -ENOMEM;
    case 0xC0000022U:
        return -EACCES;
    case 0xC0000023U:
        return -EOVERFLOW;
    case 0xC00002B6U:
        return -ENODEV;
    default:
        return -EINVAL;
    }
}

static struct hvdxg_winluid hvdxg_luid_from_guid(const struct hv_guid *guid)
{
    struct hvdxg_winluid luid;

    memset(&luid, 0, sizeof(luid));
    memcpy(&luid, guid, sizeof(luid));
    return luid;
}

static void hvdxg_set_waiting(uint64 trans_id, uint32 channel, uint32 relid)
{
    __atomic_store_n(&hvdxg.waiting_channel, channel, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_relid, relid, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_trans_id, trans_id, __ATOMIC_RELEASE);
}

static void hvdxg_clear_waiting(void)
{
    __atomic_store_n(&hvdxg.waiting_trans_id, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_relid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_channel, HV_DXG_CHANNEL_NONE,
                     __ATOMIC_RELEASE);
}

static void hvdxg_capture_completion(const uint8 *payload, uint32 payload_len,
                                     uint16 type, uint64 trans_id,
                                     uint32 source_channel,
                                     uint32 source_relid)
{
    uint64 waiting = __atomic_load_n(&hvdxg.waiting_trans_id,
                                     __ATOMIC_ACQUIRE);
    uint32 waiting_channel =
        __atomic_load_n(&hvdxg.waiting_channel, __ATOMIC_ACQUIRE);
    uint32 waiting_relid =
        __atomic_load_n(&hvdxg.waiting_relid, __ATOMIC_ACQUIRE);
    uint32 copy_len = payload_len > HV_DXG_RESULT_BYTES ?
                      HV_DXG_RESULT_BYTES : payload_len;
    uint32 prefix_len = payload_len > HV_DXG_PREFIX_BYTES ?
                        HV_DXG_PREFIX_BYTES : payload_len;

    hvdxg.probe_last_type = type;
    hvdxg.probe_last_len = payload_len;
    memset(hvdxg.probe_last_prefix, 0, sizeof(hvdxg.probe_last_prefix));
    if (prefix_len != 0)
        memcpy(hvdxg.probe_last_prefix, payload, prefix_len);

    if (waiting == 0 || trans_id != waiting ||
        waiting_channel != source_channel || waiting_relid != source_relid)
        return;

    memset(hvdxg.completion_buf, 0, sizeof(hvdxg.completion_buf));
    if (copy_len != 0)
        memcpy(hvdxg.completion_buf, payload, copy_len);
    hvdxg.completion_len = payload_len;
    hvdxg.completion_type = type;
    hvdxg.completion_trans_id = trans_id;
    __atomic_store_n(&hvdxg.completion_pending, 1, __ATOMIC_RELEASE);
}

static uint64 hvdxg_alloc_host_event_file(struct vfs_file *file,
                                          int remove_after_signal)
{
    uint64 id = __atomic_add_fetch(&hvdxg.host_event_next_id, 1,
                                   __ATOMIC_RELAXED);

    if (id == 0)
        id = __atomic_add_fetch(&hvdxg.host_event_next_id, 1,
                                __ATOMIC_RELAXED);
    for (uint32 i = 0; i < HV_DXG_HOST_EVENT_MAX; i++) {
        uint64 empty = 0;

        if (__atomic_compare_exchange_n(&hvdxg.host_event_ids[i], &empty, id,
                                        0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            hvdxg.host_event_files[i] = file;
            __atomic_store_n(&hvdxg.host_event_signaled[i], 0,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&hvdxg.host_event_remove_after_signal[i],
                             remove_after_signal ? 1 : 0,
                             __ATOMIC_RELEASE);
            __atomic_add_fetch(&hvdxg.host_event_active_count, 1,
                               __ATOMIC_RELAXED);
            __atomic_add_fetch(&hvdxg.host_event_alloc_count, 1,
                               __ATOMIC_RELAXED);
            return id;
        }
    }
    hvdxg.host_event_wait_failures++;
    return 0;
}

static uint64 hvdxg_alloc_host_event(void)
{
    return hvdxg_alloc_host_event_file(NULL, 0);
}

static void hvdxg_remove_host_event(uint64 id)
{
    struct vfs_file *file;

    if (id == 0)
        return;
    for (uint32 i = 0; i < HV_DXG_HOST_EVENT_MAX; i++) {
        if (__atomic_load_n(&hvdxg.host_event_ids[i], __ATOMIC_ACQUIRE) ==
            id) {
            file = hvdxg.host_event_files[i];
            hvdxg.host_event_files[i] = NULL;
            __atomic_store_n(&hvdxg.host_event_ids[i], 0,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&hvdxg.host_event_signaled[i], 0,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&hvdxg.host_event_remove_after_signal[i], 0,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&hvdxg.host_event_last_removed_id, id,
                             __ATOMIC_RELEASE);
            __atomic_add_fetch(&hvdxg.host_event_remove_count, 1,
                               __ATOMIC_RELAXED);
            if (__atomic_load_n(&hvdxg.host_event_active_count,
                                __ATOMIC_RELAXED) != 0)
                __atomic_sub_fetch(&hvdxg.host_event_active_count, 1,
                                   __ATOMIC_RELAXED);
            if (file != NULL)
                vfs_fput(file);
            return;
        }
    }
}

static int hvdxg_host_event_is_signaled(uint64 id)
{
    if (id == 0)
        return 0;
    for (uint32 i = 0; i < HV_DXG_HOST_EVENT_MAX; i++) {
        if (__atomic_load_n(&hvdxg.host_event_ids[i], __ATOMIC_ACQUIRE) ==
            id)
            return __atomic_load_n(&hvdxg.host_event_signaled[i],
                                   __ATOMIC_ACQUIRE) != 0;
    }
    return 0;
}

static void hvdxg_pump_events_ms(uint64 timeout_ms)
{
    for (uint64 i = 0; i < timeout_ms; i++) {
        hv_process_messages();
        hv_process_events();
        hvdxg_pump_channels();
        sleep_ms(1);
    }
}

static void hvdxg_signal_host_event(uint64 id)
{
    struct vfs_file *file;
    int remove_after_signal;

    if (id == 0)
        return;
    for (uint32 i = 0; i < HV_DXG_HOST_EVENT_MAX; i++) {
        if (__atomic_load_n(&hvdxg.host_event_ids[i], __ATOMIC_ACQUIRE) ==
            id) {
            file = hvdxg.host_event_files[i];
            remove_after_signal =
                __atomic_load_n(&hvdxg.host_event_remove_after_signal[i],
                                __ATOMIC_ACQUIRE) != 0;
            __atomic_store_n(&hvdxg.host_event_signaled[i], 1,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&hvdxg.host_event_last_id, id,
                             __ATOMIC_RELEASE);
            __atomic_add_fetch(&hvdxg.host_event_signal_count, 1,
                               __ATOMIC_RELAXED);
            if (file != NULL)
                (void)eventfd_signal_file(file, 1);
            if (remove_after_signal)
                hvdxg_remove_host_event(id);
            return;
        }
    }
}

static int hvdxg_process_host_to_vm_packet(const uint8 *payload,
                                           uint32 payload_len)
{
    const struct hvdxg_command_host_to_vm *hdr;
    const struct hvdxg_command_signalguestevent *signal;

    if (payload_len < sizeof(*hdr))
        return 0;
    hdr = (const struct hvdxg_command_host_to_vm *)payload;
    switch (hdr->command_type) {
    case HV_DXGK_VMBCOMMAND_SIGNALGUESTEVENT:
    case HV_DXGK_VMBCOMMAND_SIGNALGUESTEVENTPASSIVE:
        if (payload_len <
            sizeof(struct hvdxg_command_host_to_vm) + sizeof(uint64))
            return 1;
        signal = (const struct hvdxg_command_signalguestevent *)payload;
        hvdxg_signal_host_event(signal->event);
        return 1;
    case HV_DXGK_VMBCOMMAND_SETGUESTDATA:
    case HV_DXGK_VMBCOMMAND_SENDWNFNOTIFICATION:
        return 1;
    default:
        return 0;
    }
}

static void hvdxg_process_channel_packets(struct hv_ring_buffer *in_ring,
                                          uint32 source_channel,
                                          uint32 source_relid,
                                          uint32 *counter)
{
    uint8 *pkt = hvdxg.rx_buf;
    uint32 len;
    uint16 type;

    while (in_ring && hv_recv_raw_on(in_ring, pkt, sizeof(hvdxg.rx_buf), &len,
                                     &type) == 0) {
        if (len == 0)
            return;
        (*counter)++;
        if (len < sizeof(struct vmpacket_descriptor))
            continue;
        const struct vmpacket_descriptor *desc =
            (const struct vmpacket_descriptor *)pkt;
        uint32 off = ((uint32)desc->offset8) << 3;
        uint32 plen = ((uint32)desc->len8) << 3;
        if (off > plen || plen > len)
            continue;
        const uint8 *payload = pkt + off;
        uint32 payload_len = plen - off;
        if (type == VM_PKT_DATA_INBAND && in_ring == hvdxg.global_in_ring &&
            hvdxg_process_host_to_vm_packet(payload, payload_len))
            continue;
        if (type == VM_PKT_COMP || type == VM_PKT_DATA_INBAND) {
            uint64 waiting =
                __atomic_load_n(&hvdxg.waiting_trans_id,
                                __ATOMIC_ACQUIRE);
            uint32 waiting_channel =
                __atomic_load_n(&hvdxg.waiting_channel, __ATOMIC_ACQUIRE);
            uint32 waiting_relid =
                __atomic_load_n(&hvdxg.waiting_relid, __ATOMIC_ACQUIRE);
            uint32 waiting_match =
                waiting != 0 && desc->trans_id == waiting ? 1 : 0;
            uint32 channel_match =
                waiting_match && waiting_channel == source_channel &&
                waiting_relid == source_relid ? 1 : 0;

            hvdxg.completion_desc_type = desc->type;
            hvdxg.completion_desc_flags = desc->flags;
            hvdxg.completion_desc_len8 = desc->len8;
            hvdxg.completion_desc_offset8 = desc->offset8;
            hvdxg.completion_packet_len = plen;
            hvdxg.completion_packet_offset = off;
            hvdxg.completion_payload_len = payload_len;
            hvdxg.completion_desc_trans_id = desc->trans_id;
            hvdxg.completion_waiting_trans_id = waiting;
            hvdxg.completion_source_channel = source_channel;
            hvdxg.completion_source_relid = source_relid;
            hvdxg.completion_waiting_channel = waiting_channel;
            hvdxg.completion_waiting_relid = waiting_relid;
            hvdxg.completion_waiting_match = waiting_match;
            hvdxg.completion_waiting_channel_match = channel_match;
            if (type == VM_PKT_COMP)
                hvdxg_capture_completion(payload, payload_len, type,
                                         desc->trans_id, source_channel,
                                         source_relid);
        }
    }
}

static void hvdxg_pump_channels(void)
{
    if (__atomic_exchange_n(&hvdxg.pump_active, 1, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&hvdxg.pump_skips, 1, __ATOMIC_RELAXED);
        return;
    }
    if (hvdxg.global_open_ok)
        hvdxg_process_channel_packets(hvdxg.global_in_ring,
                                      HV_DXG_CHANNEL_GLOBAL,
                                      hvdxg.global_relid,
                                      &hvdxg.global_rx_packets);
    if (hvdxg.vgpu_open_ok)
        hvdxg_process_channel_packets(hvdxg.vgpu_in_ring,
                                      HV_DXG_CHANNEL_VGPU,
                                      hvdxg.vgpu_relid,
                                      &hvdxg.vgpu_rx_packets);
    __atomic_store_n(&hvdxg.pump_active, 0, __ATOMIC_RELEASE);
}

static void hvdxg_note_cpu_wait_state(struct hvdxg_open_state *owner,
                                      const struct hvdxg_d3dkmthandle *objects,
                                      const uint64 *fence_values,
                                      uint32 object_count, uint64 event_id,
                                      uint32 async_event, uint32 result)
{
    uint32 object = object_count != 0 && objects != NULL ?
                    objects[0].v : 0;

    hvdxg.syncwait_last_event = event_id;
    hvdxg.syncwait_last_async = async_event;
    hvdxg.syncwait_last_object = object;
    hvdxg.syncwait_last_fence =
        object_count != 0 && fence_values != NULL ? fence_values[0] : 0;
    hvdxg.syncwait_last_current =
        hvdxg_owner_sync_fence_value(owner, object);
    hvdxg.syncwait_last_result = result;
    if ((result == 1 || result == 2) && object_count != 0 &&
        objects != NULL && fence_values != NULL)
        hvdxg_note_allocation_wait(owner, object, fence_values[0], 0,
                                   result);
}

static int hvdxg_wait_host_event_or_cpu_fence(
    struct hvdxg_open_state *owner,
    const struct hvdxg_d3dkmthandle *objects,
    const uint64 *fence_values, uint32 object_count, int wait_any,
    uint64 event_id, uint64 timeout_ms)
{
    for (uint64 i = 0; i < timeout_ms; i++) {
        hvdxg_pump_events_ms(1);
        if (hvdxg_host_event_is_signaled(event_id)) {
            hvdxg.host_event_wait_successes++;
            hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                      object_count, event_id, 0, 1);
            return 0;
        }
        if (hvdxg_wait_cpu_fences_already_satisfied(
                owner, objects, fence_values, object_count, wait_any)) {
            hvdxg.host_event_wait_successes++;
            hvdxg_note_cpu_wait_state(owner, objects, fence_values,
                                      object_count, event_id, 0, 2);
            return 0;
        }
        sleep_ms(1);
    }
    hvdxg.host_event_wait_timeouts++;
    hvdxg_note_cpu_wait_state(owner, objects, fence_values, object_count,
                              event_id, 0, 3);
    return -ETIMEDOUT;
}

static int hvdxg_send_waitsyncobjectfromcpu(
    struct hvdxg_open_state *owner,
    struct d3dkmt_waitforsynchronizationobjectfromcpu *req,
    const void *objects, const void *fence_values, uint64 event_id,
    uint32 object_size, uint32 fence_size, uint32 *actual_len)
{
    uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromcpu) +
                      D3DDDI_MAX_OBJECT_WAITED_ON *
                          (sizeof(struct hvdxg_d3dkmthandle) +
                           sizeof(uint64))];
    struct hvdxg_command_waitsyncobjectfromcpu *wait =
        (struct hvdxg_command_waitsyncobjectfromcpu *)command_buf;
    struct hvdxg_ntstatus status;
    uint8 *pos;
    int ret;

    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    hvdxg.syncwait_last_status = 0;
    hvdxg_command_vgpu_init_process(
        &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMCPU,
        hvdxg_owner_bound_process_handle(owner));
    wait->device.v = req->device.v;
    wait->object_count = req->object_count;
    wait->flags = req->flags;
    wait->guest_event_pointer = event_id;
    wait->dereference_event = 0;
    pos = (uint8 *)&wait[1];
    memcpy(pos, objects, object_size);
    memcpy(pos + object_size, fence_values, fence_size);
    ret = hvdxg_send_sync_vgpu(wait, sizeof(*wait) + object_size + fence_size,
                               &status, sizeof(status), actual_len);
    if (actual_len != NULL && *actual_len >= sizeof(status))
        hvdxg.syncwait_last_status = status.v;
    if (ret == 0 && actual_len != NULL && *actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.syncwait_last_len = actual_len != NULL ? *actual_len : 0;
    hvdxg.syncwait_last_ret = ret;
    return ret;
}

static int hvdxg_wait_completion(uint64 trans_id, void *out,
                                 uint32 out_len, uint32 *actual_len,
                                 uint64 timeout_ms)
{
    for (uint32 spin = 0; spin < HV_DXG_SYNC_SPIN_POLLS; spin++) {
        hv_process_messages();
        hv_process_events();
        hvdxg_pump_channels();
        if (__atomic_load_n(&hvdxg.completion_pending, __ATOMIC_ACQUIRE) &&
            hvdxg.completion_trans_id == trans_id) {
            uint32 copy_len = hvdxg.completion_len;
            if (copy_len > out_len)
                copy_len = out_len;
            if (out != NULL && copy_len != 0)
                memcpy(out, hvdxg.completion_buf, copy_len);
            if (actual_len != NULL)
                *actual_len = hvdxg.completion_len;
            __atomic_store_n(&hvdxg.completion_pending, 0,
                             __ATOMIC_RELEASE);
            hvdxg_clear_waiting();
            return 0;
        }
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#endif
    }
    for (uint64 i = 0; i < timeout_ms; i++) {
        hv_process_messages();
        hv_process_events();
        hvdxg_pump_channels();
        if (__atomic_load_n(&hvdxg.completion_pending, __ATOMIC_ACQUIRE) &&
            hvdxg.completion_trans_id == trans_id) {
            uint32 copy_len = hvdxg.completion_len;
            if (copy_len > out_len)
                copy_len = out_len;
            if (out != NULL && copy_len != 0)
                memcpy(out, hvdxg.completion_buf, copy_len);
            if (actual_len != NULL)
                *actual_len = hvdxg.completion_len;
            __atomic_store_n(&hvdxg.completion_pending, 0,
                             __ATOMIC_RELEASE);
            hvdxg_clear_waiting();
            return 0;
        }
        sleep_ms(1);
    }
    hvdxg_clear_waiting();
    if (actual_len != NULL)
        *actual_len = 0;
    return -ETIMEDOUT;
}

static void hvdxg_capture_queryadapter_send(
    const void *cmd, uint32 cmd_len, const void *send_cmd, uint32 wire_len,
    const struct hvdxg_ext_header *ext, int route_global)
{
    const uint8 *wire = (const uint8 *)send_cmd;
    uint32 cmd_off = ext != NULL ? ext->command_offset : 0;
    uint32 type_off;
    uint32 size_off;
    uint32 data_off;

    memset(hvdxg.queryadapter_packet_ext_bytes, 0,
           sizeof(hvdxg.queryadapter_packet_ext_bytes));
    memset(hvdxg.queryadapter_packet_cmdhdr, 0,
           sizeof(hvdxg.queryadapter_packet_cmdhdr));
    memset(hvdxg.queryadapter_packet_priv_head, 0,
           sizeof(hvdxg.queryadapter_packet_priv_head));

    hvdxg.queryadapter_packet_type = 0;
    hvdxg.queryadapter_packet_size = 0;
    hvdxg.queryadapter_packet_cmd_len = cmd_len;
    hvdxg.queryadapter_packet_wire_len = wire_len;
    hvdxg.queryadapter_packet_ext = ext != NULL ? 1 : 0;
    hvdxg.queryadapter_packet_ext_offset = cmd_off;
    hvdxg.queryadapter_send_route = route_global ? 2 : 1;
    hvdxg.queryadapter_send_ext_luid_low =
        ext != NULL ? ext->vgpu_luid.a : 0;
    hvdxg.queryadapter_send_ext_luid_high =
        ext != NULL ? ext->vgpu_luid.b : 0;
    hvdxg.queryadapter_packet_desc_size = sizeof(struct vmpacket_descriptor);
    hvdxg.queryadapter_packet_len =
        hvdxg.queryadapter_packet_desc_size + wire_len;
    hvdxg.queryadapter_packet_aligned =
        (hvdxg.queryadapter_packet_len + 7U) & ~7U;
    hvdxg.queryadapter_packet_pad =
        hvdxg.queryadapter_packet_aligned - hvdxg.queryadapter_packet_len;
    hvdxg.queryadapter_packet_desc_off8 =
        hvdxg.queryadapter_packet_desc_size >> 3;
    hvdxg.queryadapter_packet_desc_len8 =
        hvdxg.queryadapter_packet_aligned >> 3;
    hvdxg.queryadapter_packet_ring_total =
        hvdxg.queryadapter_packet_aligned + sizeof(uint64);

    hvdxg.queryadapter_packet_type_offset = 0;
    hvdxg.queryadapter_packet_size_offset = 0;
    hvdxg.queryadapter_packet_data_offset = 0;
    hvdxg.queryadapter_packet_ext_len = 0;
    hvdxg.queryadapter_packet_cmdhdr_len = 0;
    hvdxg.queryadapter_packet_priv_head_len = 0;

    if (wire == NULL || cmd == NULL || cmd_off > wire_len)
        return;

    if (ext != NULL) {
        uint32 ext_len = sizeof(*ext);

        if (ext_len > wire_len)
            ext_len = wire_len;
        hvdxg.queryadapter_packet_ext_len = ext_len;
        if (ext_len > sizeof(hvdxg.queryadapter_packet_ext_bytes))
            ext_len = sizeof(hvdxg.queryadapter_packet_ext_bytes);
        memcpy(hvdxg.queryadapter_packet_ext_bytes, wire, ext_len);
    }

    type_off = cmd_off +
        __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                           query_type);
    size_off = cmd_off +
        __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                           private_data_size);
    data_off = cmd_off +
        __builtin_offsetof(struct hvdxg_command_queryadapterinfo_wsl,
                           private_data);
    hvdxg.queryadapter_packet_type_offset = type_off;
    hvdxg.queryadapter_packet_size_offset = size_off;
    hvdxg.queryadapter_packet_data_offset = data_off;

    if (cmd_off < wire_len) {
        uint32 cmdhdr_len = wire_len - cmd_off;

        if (cmdhdr_len > sizeof(hvdxg.queryadapter_packet_cmdhdr))
            cmdhdr_len = sizeof(hvdxg.queryadapter_packet_cmdhdr);
        hvdxg.queryadapter_packet_cmdhdr_len = cmdhdr_len;
        memcpy(hvdxg.queryadapter_packet_cmdhdr, wire + cmd_off, cmdhdr_len);
    }
    if (type_off + sizeof(uint32) <= wire_len)
        memcpy(&hvdxg.queryadapter_packet_type, wire + type_off,
               sizeof(uint32));
    if (size_off + sizeof(uint32) <= wire_len)
        memcpy(&hvdxg.queryadapter_packet_size, wire + size_off,
               sizeof(uint32));
    if (data_off < wire_len) {
        uint32 priv_len = wire_len - data_off;

        if (priv_len > sizeof(hvdxg.queryadapter_packet_priv_head))
            priv_len = sizeof(hvdxg.queryadapter_packet_priv_head);
        hvdxg.queryadapter_packet_priv_head_len = priv_len;
        memcpy(hvdxg.queryadapter_packet_priv_head, wire + data_off,
               priv_len);
    }
}

static void hvdxg_capture_queryadapter_completion(void)
{
    uint32 prefix_len = hvdxg.completion_len < 8 ?
                        hvdxg.completion_len : 8;

    hvdxg.queryadapter_completion_desc_type = hvdxg.completion_desc_type;
    hvdxg.queryadapter_completion_desc_flags = hvdxg.completion_desc_flags;
    hvdxg.queryadapter_completion_desc_len8 = hvdxg.completion_desc_len8;
    hvdxg.queryadapter_completion_desc_offset8 =
        hvdxg.completion_desc_offset8;
    hvdxg.queryadapter_completion_packet_len = hvdxg.completion_packet_len;
    hvdxg.queryadapter_completion_packet_offset =
        hvdxg.completion_packet_offset;
    hvdxg.queryadapter_completion_payload_len =
        hvdxg.completion_payload_len;
    hvdxg.queryadapter_completion_trans_id = hvdxg.completion_desc_trans_id;
    hvdxg.queryadapter_completion_waiting_trans_id =
        hvdxg.completion_waiting_trans_id;
    hvdxg.queryadapter_completion_source_channel =
        hvdxg.completion_source_channel;
    hvdxg.queryadapter_completion_source_relid =
        hvdxg.completion_source_relid;
    hvdxg.queryadapter_completion_waiting_channel =
        hvdxg.completion_waiting_channel;
    hvdxg.queryadapter_completion_waiting_relid =
        hvdxg.completion_waiting_relid;
    hvdxg.queryadapter_completion_waiting_match =
        hvdxg.completion_waiting_match;
    hvdxg.queryadapter_completion_waiting_channel_match =
        hvdxg.completion_waiting_channel_match;
    hvdxg.queryadapter_completion_type = hvdxg.completion_type;
    hvdxg.queryadapter_completion_len = hvdxg.completion_len;
    memset(hvdxg.queryadapter_completion_prefix, 0,
           sizeof(hvdxg.queryadapter_completion_prefix));
    if (prefix_len != 0)
        memcpy(hvdxg.queryadapter_completion_prefix,
               hvdxg.completion_buf, prefix_len);
}

static int hvdxg_should_capture_queryadapter_send(const void *cmd,
                                                  uint32 flags)
{
    const struct hvdxg_command_queryadapterinfo_wsl *query = cmd;

    if (cmd == NULL)
        return 1;
    if (query->query_type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
        (flags & HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER) &&
        hvdxg.queryadapter_packet_type ==
            HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID &&
        hvdxg.queryadapter_packet_ext)
        return 0;
    if (query->query_type == HV_DXG_QAITYPE_SELECTED_ADAPTER &&
        (flags & HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU) &&
        hvdxg.queryadapter_packet_type == HV_DXG_QAITYPE_SELECTED_ADAPTER)
        return 0;
    return 1;
}

static int hvdxg_send_sync_vgpu_flags_luid(
    const void *cmd, uint32 cmd_len, void *result, uint32 result_len,
    uint32 *actual_len, uint32 flags,
    const struct hvdxg_winluid *ext_luid)
{
    struct hvdxg_ext_header *ext = NULL;
    uint32 send_len = cmd_len;
    const void *send_cmd = cmd;
    uint64 trans_id;
    int route_global = 0;
    int ret;

    if (!hvdxg.vgpu_open_ok || hvdxg.vgpu_out_ring == NULL)
        return -ENODEV;
    if (!(flags & HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU) &&
        hvdxg.probe_async_msg_enabled &&
        hvdxg.global_open_ok && hvdxg.global_out_ring != NULL)
        route_global = 1;

    if (hvdxg.use_ext_header &&
        !(flags & HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER)) {
        send_len = cmd_len + sizeof(*ext);
        ext = kvmalloc(send_len);
        if (ext == NULL)
            return -ENOMEM;
        memset(ext, 0, sizeof(*ext));
        ext->command_offset = sizeof(*ext);
        ext->vgpu_luid = hvdxg_ext_adapter_luid(ext_luid);
        memcpy((uint8 *)ext + sizeof(*ext), cmd, cmd_len);
        send_cmd = ext;
    }
    hvdxg.vgpu_send_last_command =
        ((const struct hvdxg_command_vgpu_to_host *)cmd)->command_type;
    hvdxg.vgpu_send_last_cmd_len = cmd_len;
    hvdxg.vgpu_send_last_wire_len = send_len;
    hvdxg.vgpu_send_last_ext = ext != NULL ? 1 : 0;
    hvdxg.vgpu_send_last_ext_offset =
        ext != NULL ? ext->command_offset : 0;
    hvdxg.vgpu_send_last_process =
        ((const struct hvdxg_command_vgpu_to_host *)cmd)->process.v;
    hvdxg.vgpu_send_last_channel =
        ((const struct hvdxg_command_vgpu_to_host *)cmd)->channel_type;
    hvdxg.vgpu_send_last_route_global = route_global;
    hvdxg.vgpu_send_last_retries = 0;
    hvdxg.vgpu_send_last_ret = 0;
    if (ext != NULL)
        hvdxg.vgpu_send_last_luid = ext->vgpu_luid;
    else
        memset(&hvdxg.vgpu_send_last_luid, 0,
               sizeof(hvdxg.vgpu_send_last_luid));

    ret = hvdxg_sync_acquire();
    if (ret != 0) {
        if (ext != NULL)
            kvfree(ext);
        return ret;
    }
    trans_id = hvdxg_next_trans_id();
    __atomic_store_n(&hvdxg.completion_pending, 0, __ATOMIC_RELEASE);
    hvdxg_set_waiting(trans_id,
                      route_global ? HV_DXG_CHANNEL_GLOBAL :
                      HV_DXG_CHANNEL_VGPU,
                      route_global ? hvdxg.global_relid :
                      hvdxg.vgpu_relid);
    if (((const struct hvdxg_command_vgpu_to_host *)cmd)->command_type ==
        HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO &&
        hvdxg_should_capture_queryadapter_send(cmd, flags)) {
        hvdxg_capture_queryadapter_send(cmd, cmd_len, send_cmd, send_len,
                                        ext, route_global);
    }
    if (route_global) {
        ret = hvdxg_send_packet_with_retry(
            hvdxg.global_out_ring, hvdxg.global_relid, hvdxg.global_conn_id,
            hvdxg.global_monitor_allocated, hvdxg.global_monitorid,
            hvdxg.global_dedicated, send_cmd, send_len, trans_id,
            VM_PKT_COMPLETION_REQUESTED, &hvdxg.vgpu_send_last_retries,
            &hvdxg.vgpu_send_last_ret);
    } else {
        ret = hvdxg_send_packet_with_retry(
            hvdxg.vgpu_out_ring, hvdxg.vgpu_relid, hvdxg.vgpu_conn_id,
            hvdxg.vgpu_monitor_allocated, hvdxg.vgpu_monitorid,
            hvdxg.vgpu_dedicated, send_cmd, send_len, trans_id,
            VM_PKT_COMPLETION_REQUESTED, &hvdxg.vgpu_send_last_retries,
            &hvdxg.vgpu_send_last_ret);
    }
    if (ret != 0) {
        hvdxg_clear_waiting();
        hvdxg_sync_release();
        if (ext != NULL)
            kvfree(ext);
        return ret;
    }
    hvdxg_note_host_command(
        ((const struct hvdxg_command_vgpu_to_host *)cmd)->command_type);
    ret = hvdxg_wait_completion(trans_id, result, result_len, actual_len,
                                HV_DXG_WAIT_MS);
    hvdxg_sync_release();
    if (ext != NULL)
        kvfree(ext);
    return ret;
}

static int hvdxg_send_sync_vgpu_flags(const void *cmd, uint32 cmd_len,
                                      void *result, uint32 result_len,
                                      uint32 *actual_len, uint32 flags)
{
    return hvdxg_send_sync_vgpu_flags_luid(cmd, cmd_len, result,
                                           result_len, actual_len, flags,
                                           NULL);
}

static int hvdxg_send_sync_vgpu(const void *cmd, uint32 cmd_len,
                                void *result, uint32 result_len,
                                uint32 *actual_len)
{
    return hvdxg_send_sync_vgpu_flags(cmd, cmd_len, result, result_len,
                                      actual_len, 0);
}

static void hvdxg_record_global_send_diag(
    struct hvdxg_global_send_diag *diag,
    const struct hvdxg_command_vm_to_host *vmcmd, uint32 cmd_len,
    uint32 wire_len, uint32 result_len, const struct hvdxg_ext_header *ext)
{
    diag->command_id = vmcmd->command_id;
    diag->command = vmcmd->command_type;
    diag->cmd_len = cmd_len;
    diag->wire_len = wire_len;
    diag->result_len = result_len;
    diag->ext = ext != NULL ? 1 : 0;
    diag->ext_offset = ext != NULL ? ext->command_offset : 0;
    diag->process = vmcmd->process.v;
    diag->channel = vmcmd->channel_type;
    diag->relid = hvdxg.global_relid;
    diag->conn_id = hvdxg.global_conn_id;
    diag->monitor_allocated = hvdxg.global_monitor_allocated;
    diag->monitorid = hvdxg.global_monitorid;
    diag->dedicated = hvdxg.global_dedicated;
    diag->luid = hvdxg.global_send_last_luid;
}

static int hvdxg_send_sync_global_ex(const void *cmd, uint32 cmd_len,
                                     void *result, uint32 result_len,
                                     uint32 *actual_len, int force_ext_header,
                                     int ext_host_vgpu_luid,
                                     int suppress_ext_header)
{
    struct hvdxg_ext_header *ext = NULL;
    const struct hvdxg_command_vm_to_host *vmcmd =
        (const struct hvdxg_command_vm_to_host *)cmd;
    uint32 send_len = cmd_len;
    const void *send_cmd = cmd;
    uint64 trans_id;
    int ret;

    if (!hvdxg.global_open_ok || hvdxg.global_out_ring == NULL)
        return -ENODEV;

    if (!suppress_ext_header &&
        (hvdxg.use_ext_header || force_ext_header)) {
        send_len = cmd_len + sizeof(*ext);
        ext = kvmalloc(send_len);
        if (ext == NULL)
            return -ENOMEM;
        memset(ext, 0, sizeof(*ext));
        ext->command_offset = sizeof(*ext);
        if (ext_host_vgpu_luid)
            ext->vgpu_luid = hvdxg_ext_adapter_luid(NULL);
        memcpy((uint8 *)ext + sizeof(*ext), cmd, cmd_len);
        send_cmd = ext;
    }
    hvdxg.global_send_last_command = vmcmd->command_type;
    hvdxg.global_send_last_cmd_len = cmd_len;
    hvdxg.global_send_last_wire_len = send_len;
    hvdxg.global_send_last_ext = ext != NULL ? 1 : 0;
    hvdxg.global_send_last_ext_offset =
        ext != NULL ? ext->command_offset : 0;
    hvdxg.global_send_last_process = vmcmd->process.v;
    hvdxg.global_send_last_channel = vmcmd->channel_type;
    hvdxg.global_send_last_retries = 0;
    hvdxg.global_send_last_ret = 0;
    if (ext != NULL)
        hvdxg.global_send_last_luid = ext->vgpu_luid;
    else
        memset(&hvdxg.global_send_last_luid, 0,
               sizeof(hvdxg.global_send_last_luid));
    switch (vmcmd->command_type) {
    case HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT:
        hvdxg_record_global_send_diag(&hvdxg.global_send_ntshared,
                                      vmcmd, cmd_len, send_len,
                                      result_len, ext);
        if (ext != NULL)
            hvdxg_record_global_send_diag(
                &hvdxg.global_send_ntshared_ext, vmcmd, cmd_len,
                send_len, result_len, ext);
        break;
    case HV_DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST:
        hvdxg_record_global_send_diag(&hvdxg.global_send_shareobject,
                                      vmcmd, cmd_len, send_len,
                                      result_len, ext);
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT:
        hvdxg_record_global_send_diag(&hvdxg.global_send_destroynt,
                                      vmcmd, cmd_len, send_len,
                                      result_len, ext);
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT:
        hvdxg_record_global_send_diag(&hvdxg.global_send_destroysync,
                                      vmcmd, cmd_len, send_len,
                                      result_len, ext);
        break;
    default:
        break;
    }

    ret = hvdxg_sync_acquire();
    if (ret != 0) {
        if (ext != NULL)
            kvfree(ext);
        return ret;
    }
    trans_id = hvdxg_next_trans_id();
    __atomic_store_n(&hvdxg.completion_pending, 0, __ATOMIC_RELEASE);
    hvdxg_set_waiting(trans_id, HV_DXG_CHANNEL_GLOBAL, hvdxg.global_relid);
    ret = hvdxg_send_packet_with_retry(
        hvdxg.global_out_ring, hvdxg.global_relid, hvdxg.global_conn_id,
        hvdxg.global_monitor_allocated, hvdxg.global_monitorid,
        hvdxg.global_dedicated, send_cmd, send_len, trans_id,
        VM_PKT_COMPLETION_REQUESTED, &hvdxg.global_send_last_retries,
        &hvdxg.global_send_last_ret);
    if (ret != 0) {
        hvdxg_clear_waiting();
        hvdxg_sync_release();
        if (ext != NULL)
            kvfree(ext);
        return ret;
    }
    hvdxg_note_host_command(
        ((const struct hvdxg_command_vm_to_host *)cmd)->command_type);
    ret = hvdxg_wait_completion(trans_id, result, result_len, actual_len,
                                HV_DXG_WAIT_MS);
    hvdxg_sync_release();
    if (ext != NULL)
        kvfree(ext);
    return ret;
}

static int hvdxg_send_sync_global(const void *cmd, uint32 cmd_len,
                                  void *result, uint32 result_len,
                                  uint32 *actual_len)
{
    return hvdxg_send_sync_global_ex(cmd, cmd_len, result, result_len,
                                     actual_len, 0, 0, 0);
}

static int hvdxg_destroy_process_host(struct hvdxg_d3dkmthandle process)
{
    struct hvdxg_command_destroyprocess_wsl cmd_wsl;
    uint32 actual_len = 0;
    int ret;

    if (process.v == 0)
        return 0;
    memset(&cmd_wsl, 0, sizeof(cmd_wsl));
    hvdxg_command_vm_init(&cmd_wsl.hdr, HV_DXGK_VMBCOMMAND_DESTROYPROCESS);
    cmd_wsl.hdr.process = process;
    hvdxg.destroyprocess_last_len = 0;
    hvdxg.destroyprocess_last_ret = 0;
    hvdxg.destroyprocess_last_handle = process.v;
    hvdxg.process_destroy_attempts++;
    ret = hvdxg_send_sync_global(&cmd_wsl, sizeof(cmd_wsl), NULL, 0,
                                 &actual_len);
    hvdxg.destroyprocess_last_len = actual_len;
    hvdxg.destroyprocess_last_ret = ret;
    hvdxg.destroyprocess_last_handle = process.v;
    if (ret == 0)
        hvdxg.process_destroy_successes++;
    else
        hvdxg.process_destroy_failures++;
    return ret;
}

static int hvdxg_create_process(void)
{
    struct hvdxg_command_createprocess_wsl cmd_wsl;
    struct hvdxg_command_createprocess_return result;
    uint32 actual_len = 0;
    const char *name = current ? current->name : "xv6-dxg";
    uint64 current_pid = current ? (uint64)current->pid : 1;
    uint64 current_tgid = current ? (uint64)thread_tgid(current) :
                          current_pid;
    uint64 guest_process = hvdxg.dxg_process_guest != 0 ?
                           hvdxg.dxg_process_guest :
                           (current ? (uint64)current : 1);
    uint64 process_id = hvdxg.dxg_process_pid != 0 ?
                        hvdxg.dxg_process_pid :
                        current_pid;
    uint64 process_tgid = hvdxg.dxg_process_tgid != 0 ?
                          hvdxg.dxg_process_tgid : current_tgid;
    int ret;

    if (hvdxg.dxg_process_created && hvdxg.dxg_process.v != 0)
        return 0;

    hvdxg.createprocess_last_len = 0;
    hvdxg.createprocess_last_cmd_len = sizeof(cmd_wsl);
    hvdxg.createprocess_last_ret = 0;
    hvdxg.createprocess_last_guest = guest_process;
    hvdxg.createprocess_last_pid = process_id;
    hvdxg.createprocess_last_tgid = process_tgid;
    hvdxg.createprocess_last_handle = 0;
    hvdxg.createprocess_last_layout = 1;
    hvdxg.createprocess_last_generation = hvdxg.dxg_process_generation;

    memset(&cmd_wsl, 0, sizeof(cmd_wsl));
    hvdxg_command_vm_init(&cmd_wsl.hdr, HV_DXGK_VMBCOMMAND_CREATEPROCESS);
    cmd_wsl.process = guest_process;
    cmd_wsl.process_id = process_id;
    for (uint32 i = 0; i < HV_DXG_PROCESS_NAME_LENGTH && name[i] != 0; i++)
        cmd_wsl.process_name[i] = (uint16)name[i];
    cmd_wsl.flags = 0x8;

    memset(&result, 0, sizeof(result));
    actual_len = 0;
    ret = hvdxg_send_sync_global(&cmd_wsl, sizeof(cmd_wsl), &result,
                                 sizeof(result), &actual_len);
    hvdxg.createprocess_last_len = actual_len;
    hvdxg.createprocess_last_cmd_len = sizeof(cmd_wsl);
    hvdxg.createprocess_last_ret = ret;
    hvdxg.createprocess_last_handle = result.hprocess.v;
    hvdxg.createprocess_last_layout = 1;
    if (ret != 0) {
        hvdxg.dxg_process.v = 0;
        hvdxg.dxg_process_created = 0;
        hvdxg.d3dkmt_ready = 0;
        return ret;
    }
    if (actual_len < sizeof(result) || result.hprocess.v == 0) {
        hvdxg.createprocess_last_ret = -EIO;
        hvdxg.dxg_process.v = 0;
        hvdxg.dxg_process_created = 0;
        hvdxg.d3dkmt_ready = 0;
        return -EIO;
    }
    hvdxg.dxg_process = result.hprocess;
    hvdxg.dxg_process_created = 1;
    hvdxg.dxg_process_guest = guest_process;
    hvdxg.dxg_process_pid = process_id;
    hvdxg.dxg_process_tgid = process_tgid;
    hvdxg.createprocess_success_len = actual_len;
    hvdxg.createprocess_success_cmd_len = sizeof(cmd_wsl);
    hvdxg.createprocess_success_ret = ret;
    hvdxg.createprocess_success_guest = guest_process;
    hvdxg.createprocess_success_pid = process_id;
    hvdxg.createprocess_success_tgid = process_tgid;
    hvdxg.createprocess_success_handle = result.hprocess.v;
    hvdxg.createprocess_success_layout = 1;
    hvdxg.createprocess_success_generation = hvdxg.dxg_process_generation;
    return 0;
}

static int hvdxg_d3dkmt_ensure_adapter(void)
{
    int ret;

    if (!hvdxg.global_open_ok || !hvdxg.vgpu_open_ok)
        return -ENODEV;
    if (hvdxg.probe_successes == 0) {
        ret = hvdxg_probe_transport();
        if (ret != 0)
            return ret;
    }
    if (hvdxg.host_adapter_handle == 0 && hvdxg.probe_open_handle != 0)
        hvdxg.host_adapter_handle = hvdxg.probe_open_handle;
    if (hvdxg.adapter_luid.a == 0 && hvdxg.adapter_luid.b == 0)
        hvdxg.adapter_luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);
    return 0;
}

static int hvdxg_d3dkmt_ensure(void)
{
    int ret;

    ret = hvdxg_d3dkmt_ensure_adapter();
    if (ret != 0)
        return ret;
    ret = hvdxg_create_process();
    if (ret != 0)
        return ret;
    if (!hvdxg.iospace_set)
        (void)hvdxg_set_iospace_region();
    hvdxg.d3dkmt_ready = 1;
    return 0;
}

static int hvdxg_probe_transport(void)
{
    struct hvdxg_command_openadapter open;
    struct hvdxg_command_openadapter_return open_ret;
    struct hvdxg_command_getinternaladapterinfo info;
    struct hvdxg_internal_adapter_info_return info_ret;
    uint32 actual_len = 0;
    uint32 info_result_len;
    uint32 versions[2];
    uint32 version_count = 0;
    int ret;

    if (hvdxg_try_pci_guestcaps_scan() == -ENODEV)
        hvdxg_note_missing_pci_guestcaps_once();
    if (!hvdxg.vgpu_open_ok)
        return -ENODEV;
    if (!hvdxg.global_open_ok)
        return -ENODEV;

    if (hvdxg.active_vmbus_version == 0) {
        uint32 initial_version = hvdxg_host_v40_signal() ?
            HV_DXG_VMBUS_INTERFACE_VERSION :
            HV_DXG_VMBUS_INTERFACE_VERSION_OLD;

        hvdxg_set_active_vmbus_version(
            initial_version, 0,
            HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
    }
    if (hvdxg_host_v40_signal())
        versions[version_count++] = HV_DXG_VMBUS_INTERFACE_VERSION;
    versions[version_count++] = HV_DXG_VMBUS_INTERFACE_VERSION_OLD;

    hvdxg.probe_attempts++;
    hvdxg.probe_last_ret = -EIO;
    hvdxg.probe_open_status = 0;
    hvdxg.probe_open_handle = 0;
    hvdxg.probe_open_requested_version = 0;
    hvdxg.probe_open_host_version = 0;
    hvdxg.probe_open_host_compat = 0;
    hvdxg.probe_info_len = 0;
    hvdxg.probe_info_flags = 0;
    hvdxg.probe_async_msg_enabled = 0;
    hvdxg.probe_v40_open_send_ret = 0;
    hvdxg.probe_v40_open_actual_len = 0;
    hvdxg.probe_v40_open_status = 0;
    hvdxg.probe_v40_open_handle = 0;
    hvdxg.probe_v40_open_host_version = 0;
    hvdxg.probe_v40_open_host_compat = 0;
    hvdxg.probe_v40_open_guest_luid_low = 0;
    hvdxg.probe_v40_open_guest_luid_high = 0;
    hvdxg.probe_v40_getinternal_send_ret = 0;
    hvdxg.probe_v40_getinternal_actual_len = 0;
    hvdxg.probe_v40_getinternal_flags = 0;
    hvdxg.probe_v40_reject_reason =
        HV_DXG_PROBE_V40_REJECT_NOT_ATTEMPTED;
    hvdxg.use_ext_header = 0;
    hvdxg_apply_cmdline_host_luid();

    ret = hvdxg_set_iospace_region();
    hvdxg.probe_last_ret = ret;
    if (ret != 0)
        return ret;
    for (uint32 i = 0; i < version_count; i++) {
        uint32 requested_version = versions[i];
        uint32 source = requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION ?
                        3 : 4;

        hvdxg.probe_async_msg_enabled = 0;
        hvdxg_set_active_vmbus_version(
            requested_version, source,
            HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
        memset(&open, 0, sizeof(open));
        hvdxg_command_vgpu_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENADAPTER);
        open.vmbus_interface_version = requested_version;
        hvdxg.probe_open_requested_version = requested_version;
        open.vmbus_last_compatible_interface_version =
            HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;
        if (requested_version < HV_DXG_VMBUS_INTERFACE_VERSION)
            open.guest_adapter_luid =
                hvdxg_luid_from_guid(&hvdxg.vgpu_instance);
        if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
            hvdxg.probe_v40_open_guest_luid_low =
                open.guest_adapter_luid.a;
            hvdxg.probe_v40_open_guest_luid_high =
                open.guest_adapter_luid.b;
        }

        memset(&open_ret, 0, sizeof(open_ret));
        actual_len = 0;
        ret = hvdxg_send_sync_vgpu(&open, sizeof(open), &open_ret,
                                   sizeof(open_ret), &actual_len);
        hvdxg.probe_last_ret = ret;
        if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
            hvdxg.probe_v40_open_send_ret = ret;
            hvdxg.probe_v40_open_actual_len = actual_len;
            hvdxg.probe_v40_open_status = open_ret.status.v;
            hvdxg.probe_v40_open_handle =
                open_ret.host_adapter_handle.v;
            hvdxg.probe_v40_open_host_version =
                open_ret.vmbus_interface_version;
            hvdxg.probe_v40_open_host_compat =
                open_ret.vmbus_last_compatible_interface_version;
            hvdxg.probe_v40_reject_reason =
                HV_DXG_PROBE_V40_REJECT_OPEN_SEND;
        }
        if (ret != 0) {
            if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION)
                hvdxg.active_vmbus_fallbacks++;
            continue;
        }
        if (actual_len < sizeof(open_ret)) {
            hvdxg.probe_last_ret = -EOVERFLOW;
            ret = -EOVERFLOW;
            if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
                hvdxg.probe_v40_reject_reason =
                    HV_DXG_PROBE_V40_REJECT_OPEN_SHORT;
                hvdxg.active_vmbus_fallbacks++;
            }
            continue;
        }

        hvdxg.probe_open_status = open_ret.status.v;
        hvdxg.probe_open_handle = open_ret.host_adapter_handle.v;
        hvdxg.probe_open_host_version = open_ret.vmbus_interface_version;
        hvdxg.probe_open_host_compat =
            open_ret.vmbus_last_compatible_interface_version;
        if (hvdxg_luid_nonzero(open.guest_adapter_luid))
            hvdxg.adapter_luid = open.guest_adapter_luid;
        else
            hvdxg.adapter_luid =
                hvdxg_luid_from_guid(&hvdxg.vgpu_instance);
        hvdxg.host_adapter_handle = open_ret.host_adapter_handle.v;
        if (open_ret.status.v < 0) {
            hvdxg.probe_last_ret = open_ret.status.v;
            ret = -EIO;
            if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
                hvdxg.probe_v40_reject_reason =
                    HV_DXG_PROBE_V40_REJECT_OPEN_STATUS;
                hvdxg.active_vmbus_fallbacks++;
            }
            continue;
        }
        if (open_ret.host_adapter_handle.v == 0) {
            hvdxg.probe_last_ret = -EIO;
            ret = -EIO;
            if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
                hvdxg.probe_v40_reject_reason =
                    HV_DXG_PROBE_V40_REJECT_OPEN_ZERO_HANDLE;
                hvdxg.active_vmbus_fallbacks++;
            }
            continue;
        }

        memset(&info, 0, sizeof(info));
        hvdxg_command_vgpu_init(&info.hdr,
                                HV_DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO);
        memset(&info_ret, 0, sizeof(info_ret));
        actual_len = 0;
        info_result_len = sizeof(info_ret);
        if (requested_version < HV_DXG_VMBUS_INTERFACE_VERSION)
            info_result_len -= sizeof(struct hvdxg_winluid);
        ret = hvdxg_send_sync_vgpu(&info, sizeof(info), &info_ret,
                                   info_result_len, &actual_len);
        hvdxg.probe_last_ret = ret;
        if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
            hvdxg.probe_v40_getinternal_send_ret = ret;
            hvdxg.probe_v40_getinternal_actual_len = actual_len;
            hvdxg.probe_v40_getinternal_flags =
                actual_len >= sizeof(uint32) * 4 ? info_ret.flags : 0;
            hvdxg.probe_v40_reject_reason =
                HV_DXG_PROBE_V40_REJECT_GETINTERNAL_SEND;
        }
        if (ret != 0) {
            hvdxg.use_ext_header = 0;
            if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION)
                hvdxg.active_vmbus_fallbacks++;
            continue;
        }
        if (actual_len <
                offsetof(struct hvdxg_internal_adapter_info_return,
                         device_description)) {
            hvdxg.probe_last_ret = -EOVERFLOW;
            ret = -EOVERFLOW;
            hvdxg.use_ext_header = 0;
            if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION) {
                hvdxg.probe_v40_reject_reason =
                    HV_DXG_PROBE_V40_REJECT_GETINTERNAL_SHORT;
                hvdxg.active_vmbus_fallbacks++;
            }
            continue;
        }
        if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION &&
            actual_len <
                offsetof(struct hvdxg_internal_adapter_info_return,
                         device_instance_id)) {
            hvdxg.probe_last_ret = -EOVERFLOW;
            ret = -EOVERFLOW;
            hvdxg.use_ext_header = 0;
            hvdxg.probe_v40_reject_reason =
                HV_DXG_PROBE_V40_REJECT_GETINTERNAL_SHORT;
            hvdxg.active_vmbus_fallbacks++;
            continue;
        }

        hvdxg.probe_info_len = actual_len;
        hvdxg.probe_info_flags = info_ret.flags;
        hvdxg.probe_async_msg_enabled = (info_ret.flags >> 6) & 1U;
        hvdxg.host_adapter_luid = info_ret.host_adapter_luid;
        if (actual_len >=
                offsetof(struct hvdxg_internal_adapter_info_return,
                         host_vgpu_luid) +
                    sizeof(info_ret.host_vgpu_luid) &&
            hvdxg_luid_nonzero(info_ret.host_vgpu_luid))
            hvdxg.host_vgpu_luid = info_ret.host_vgpu_luid;
        else if (hvdxg_luid_nonzero(hvdxg.pci_host_vgpu_luid))
            hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
        hvdxg_set_active_vmbus_version(
            requested_version, source,
            open_ret.vmbus_last_compatible_interface_version);
        if (requested_version >= HV_DXG_VMBUS_INTERFACE_VERSION)
            hvdxg.probe_v40_reject_reason =
                HV_DXG_PROBE_V40_REJECT_NONE;
        ret = 0;
        break;
    }
    if (ret != 0) {
        hvdxg_set_active_vmbus_version(HV_DXG_VMBUS_INTERFACE_VERSION_OLD,
                                       4,
                                       HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
        return ret;
    }

    hvdxg.probe_successes++;
    return 0;
}

static void hv_process_events(void)
{
    if (hv.child_relid != 0 && hv_test_and_clear_event(hv.child_relid))
        hv_process_channel_packets();
    if (hvkbd.open_ok && hvkbd.child_relid != 0 &&
        hv_test_and_clear_event(hvkbd.child_relid))
        hvkbd_process_channel_packets();
    if (hvstor.open_ok && hvstor.child_relid != 0 &&
        hv_test_and_clear_event(hvstor.child_relid))
        hvstor_process_channel_packets();
    if (hvnet.open_ok && hvnet.child_relid != 0 &&
        hv_test_and_clear_event(hvnet.child_relid))
        hvnet_process_channel_packets();
    if (hvvideo.open_ok && hvvideo.child_relid != 0 &&
        hv_test_and_clear_event(hvvideo.child_relid))
        hvvideo_process_channel_packets();
    if (hvpci.open_ok && hvpci.child_relid != 0 &&
        hv_test_and_clear_event(hvpci.child_relid))
        hvpci_process_channel_packets();
    if (hvdxg.global_open_ok && hvdxg.global_relid != 0 &&
        hv_test_and_clear_event(hvdxg.global_relid))
        hvdxg_pump_channels();
    if (hvdxg.vgpu_open_ok && hvdxg.vgpu_relid != 0 &&
        hv_test_and_clear_event(hvdxg.vgpu_relid))
        hvdxg_pump_channels();
}
