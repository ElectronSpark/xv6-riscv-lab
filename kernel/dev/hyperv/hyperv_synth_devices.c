void hyperv_input_intr(void)
{
    hv_process_messages();
    hv_process_events();
}

static int hv_wait_flag(volatile int *flag)
{
    for (int i = 0; i < HV_WAIT_LOOPS; i++) {
        hv_process_messages();
        hv_process_events();
        if (hv.open_ok)
            hv_process_channel_packets();
        if (hvkbd.open_ok)
            hvkbd_process_channel_packets();
        if (hvstor.open_ok)
            hvstor_process_channel_packets();
        if (hvnet.open_ok)
            hvnet_process_channel_packets();
        if (hvvideo.open_ok)
            hvvideo_process_channel_packets();
        if (hvpci.open_ok)
            hvpci_process_channel_packets();
        if (*flag)
            return 0;
        sleep_ms(10);
    }
    return -ETIMEDOUT;
}

static void hv_ring_init(void)
{
    hv.out_ring = (struct hv_ring_buffer *)hv.ring;
    hv.in_ring = (struct hv_ring_buffer *)(hv.ring + HV_SEND_PAGES * PGSIZE);

    memset(hv.ring, 0, HV_RING_PAGES * PGSIZE);
    hv.out_ring->feature_bits = 1;
    hv.in_ring->feature_bits = 1;
}

static void hvkbd_ring_init(void)
{
    hvkbd.out_ring = (struct hv_ring_buffer *)hvkbd.ring;
    hvkbd.in_ring = (struct hv_ring_buffer *)(hvkbd.ring + HV_SEND_PAGES * PGSIZE);

    memset(hvkbd.ring, 0, HV_RING_PAGES * PGSIZE);
    hvkbd.out_ring->feature_bits = 1;
    hvkbd.in_ring->feature_bits = 1;
}

static void hvstor_ring_init(void)
{
    hvstor.out_ring = (struct hv_ring_buffer *)hvstor.ring;
    hvstor.in_ring =
        (struct hv_ring_buffer *)(hvstor.ring + HV_SEND_PAGES * PGSIZE);

    memset(hvstor.ring, 0, HV_RING_PAGES * PGSIZE);
    hvstor.out_ring->feature_bits = 1;
    hvstor.in_ring->feature_bits = 1;
}

static void hvnet_ring_init(void)
{
    hvnet.out_ring = (struct hv_ring_buffer *)hvnet.ring;
    hvnet.in_ring =
        (struct hv_ring_buffer *)(hvnet.ring + HV_SEND_PAGES * PGSIZE);

    memset(hvnet.ring, 0, HV_RING_PAGES * PGSIZE);
    hvnet.out_ring->feature_bits = 1;
    hvnet.in_ring->feature_bits = 1;
}

static void hvvideo_ring_init(void)
{
    hvvideo.out_ring = (struct hv_ring_buffer *)hvvideo.ring;
    hvvideo.in_ring =
        (struct hv_ring_buffer *)(hvvideo.ring + HV_SEND_PAGES * PGSIZE);

    memset(hvvideo.ring, 0, HV_RING_PAGES * PGSIZE);
    hvvideo.out_ring->feature_bits = 1;
    hvvideo.in_ring->feature_bits = 1;
}

static void hvpci_ring_init(void)
{
    hvpci.out_ring = (struct hv_ring_buffer *)hvpci.ring;
    hvpci.in_ring =
        (struct hv_ring_buffer *)(hvpci.ring + HV_SEND_PAGES * PGSIZE);

    memset(hvpci.ring, 0, HV_RING_PAGES * PGSIZE);
    hvpci.out_ring->feature_bits = 1;
    hvpci.in_ring->feature_bits = 1;
}

static uint32 hv_ring_datasize(struct hv_ring_buffer *ring, uint32 pages)
{
    (void)ring;
    return pages * PGSIZE - sizeof(struct hv_ring_buffer);
}

static void hv_ring_copy_in(struct hv_ring_buffer *ring, uint32 pages,
                            uint32 off, const void *src, uint32 len)
{
    uint32 size = hv_ring_datasize(ring, pages);
    const uint8 *p = src;
    uint8 *buf = ring->buffer;
    for (uint32 i = 0; i < len; i++)
        buf[(off + i) % size] = p[i];
}

static void hv_ring_copy_out(struct hv_ring_buffer *ring, uint32 pages,
                             uint32 off, void *dst, uint32 len)
{
    uint32 size = hv_ring_datasize(ring, pages);
    uint8 *p = dst;
    uint8 *buf = ring->buffer;
    for (uint32 i = 0; i < len; i++)
        p[i] = buf[(off + i) % size];
}

static uint32 hv_ring_bytes_to_read(struct hv_ring_buffer *ring, uint32 pages)
{
    uint32 size = hv_ring_datasize(ring, pages);
    uint32 read = ring->read_index;
    uint32 write = __atomic_load_n(&ring->write_index, __ATOMIC_ACQUIRE);
    return write >= read ? write - read : (size - read) + write;
}

static int hv_send_packet_type_on(struct hv_ring_buffer *out_ring,
                                  uint32 child_relid, uint32 signal_conn_id,
                                  int monitor_allocated, uint8 monitorid,
                                  int dedicated, uint16 packet_type,
                                  const void *payload, uint32 payload_len,
                                  uint64 trans_id, uint32 flags)
{
    struct vmpacket_descriptor desc;
    uint64 pad = 0;
    uint64 prev;
    uint32 packet_len = sizeof(desc) + payload_len;
    uint32 aligned = (packet_len + 7) & ~7U;
    uint32 total = aligned + sizeof(uint64);
    uint32 size = hv_ring_datasize(out_ring, HV_SEND_PAGES);
    uint32 read = __atomic_load_n(&out_ring->read_index, __ATOMIC_ACQUIRE);
    uint32 write = out_ring->write_index;
    uint32 old_write = write;
    uint32 avail = write >= read ? size - (write - read) : read - write;

    if (avail <= total)
        return -EAGAIN;

    memset(&desc, 0, sizeof(desc));
    desc.type = packet_type;
    desc.flags = flags;
    desc.offset8 = sizeof(desc) >> 3;
    desc.len8 = aligned >> 3;
    desc.trans_id = trans_id;

    hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, &desc, sizeof(desc));
    write = (write + sizeof(desc)) % size;
    hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, payload, payload_len);
    write = (write + payload_len) % size;
    if (aligned > packet_len) {
        hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, &pad,
                        aligned - packet_len);
        write = (write + aligned - packet_len) % size;
    }
    prev = ((uint64)out_ring->write_index) << 32;
    hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, &prev, sizeof(prev));
    write = (write + sizeof(prev)) % size;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    out_ring->write_index = write;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (hv_ring_should_signal(out_ring, old_write))
        hv_signal_channel(child_relid, signal_conn_id, monitor_allocated,
                          monitorid, dedicated);
    return 0;
}

static int hv_send_packet_on(struct hv_ring_buffer *out_ring,
                             uint32 child_relid, uint32 signal_conn_id,
                             int monitor_allocated, uint8 monitorid,
                             int dedicated, const void *payload,
                             uint32 payload_len, uint64 trans_id, uint32 flags)
{
    return hv_send_packet_type_on(out_ring, child_relid, signal_conn_id,
                                  monitor_allocated, monitorid, dedicated,
                                  VM_PKT_DATA_INBAND, payload, payload_len,
                                  trans_id, flags);
}

static int hv_send_packet_mpb_on(struct hv_ring_buffer *out_ring,
                                 uint32 child_relid, uint32 signal_conn_id,
                                 int monitor_allocated, uint8 monitorid,
                                 int dedicated,
                                 const struct hv_multipage_buffer *mpb,
                                 const void *payload, uint32 payload_len,
                                 uint64 trans_id)
{
    struct vmbus_packet_multipage_buffer desc;
    uint64 pad = 0;
    uint64 prev;
    uint32 pfn_count = (mpb->offset + mpb->len + PGSIZE - 1) >> PGSHIFT;
    if (pfn_count == 0 || pfn_count > HV_STOR_MAX_PAGES)
        return -EINVAL;

    uint32 desc_len = sizeof(desc) -
        (HV_STOR_MAX_PAGES - pfn_count) * sizeof(uint64);
    uint32 packet_len = desc_len + payload_len;
    uint32 aligned = (packet_len + 7) & ~7U;
    uint32 total = aligned + sizeof(uint64);
    uint32 size = hv_ring_datasize(out_ring, HV_SEND_PAGES);
    uint32 read = __atomic_load_n(&out_ring->read_index, __ATOMIC_ACQUIRE);
    uint32 write = out_ring->write_index;
    uint32 old_write = write;
    uint32 avail = write >= read ? size - (write - read) : read - write;

    if (avail <= total)
        return -EAGAIN;

    memset(&desc, 0, sizeof(desc));
    desc.type = VM_PKT_DATA_USING_GPA_DIRECT;
    desc.flags = VM_PKT_COMPLETION_REQUESTED;
    desc.offset8 = desc_len >> 3;
    desc.len8 = aligned >> 3;
    desc.trans_id = trans_id;
    desc.range_count = 1;
    desc.range.len = mpb->len;
    desc.range.offset = mpb->offset;
    for (uint32 i = 0; i < pfn_count; i++)
        desc.range.pfn_array[i] = mpb->pfn_array[i];

    hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, &desc, desc_len);
    write = (write + desc_len) % size;
    hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, payload, payload_len);
    write = (write + payload_len) % size;
    if (aligned > packet_len) {
        hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, &pad,
                        aligned - packet_len);
        write = (write + aligned - packet_len) % size;
    }
    prev = ((uint64)out_ring->write_index) << 32;
    hv_ring_copy_in(out_ring, HV_SEND_PAGES, write, &prev, sizeof(prev));
    write = (write + sizeof(prev)) % size;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    out_ring->write_index = write;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    if (hv_ring_should_signal(out_ring, old_write))
        hv_signal_channel(child_relid, signal_conn_id, monitor_allocated,
                          monitorid, dedicated);
    return 0;
}

static int hv_send_packet(const void *payload, uint32 payload_len, uint64 trans_id,
                          uint32 flags)
{
    return hv_send_packet_on(hv.out_ring, hv.child_relid, hv.signal_conn_id,
                             hv.monitor_allocated, hv.monitorid,
                             hv.dedicated, payload, payload_len, trans_id,
                             flags);
}

static int hv_recv_raw_on(struct hv_ring_buffer *in_ring, void *buf,
                          uint32 buflen, uint32 *out_len, uint16 *out_type)
{
    struct vmpacket_descriptor desc;
    uint32 avail = hv_ring_bytes_to_read(in_ring, HV_RECV_PAGES);
    uint32 size = hv_ring_datasize(in_ring, HV_RECV_PAGES);
    uint32 read = in_ring->read_index;

    *out_len = 0;
    *out_type = 0;
    if (avail < sizeof(desc))
        return 0;

    hv_ring_copy_out(in_ring, HV_RECV_PAGES, read, &desc, sizeof(desc));
    uint32 pkt_len = ((uint32)desc.len8) << 3;
    uint32 pkt_off = ((uint32)desc.offset8) << 3;
    if (pkt_len < sizeof(desc) || pkt_len > avail || pkt_off > pkt_len) {
        in_ring->read_index = (read + avail) % size;
        return -EINVAL;
    }

    uint32 copy_len = pkt_len > buflen ? buflen : pkt_len;
    hv_ring_copy_out(in_ring, HV_RECV_PAGES, read, buf, copy_len);
    in_ring->read_index = (read + pkt_len + 8) % size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    *out_len = copy_len;
    *out_type = desc.type;
    (void)pkt_off;
    return 0;
}

static int hv_recv_raw(void *buf, uint32 buflen, uint32 *out_len,
                       uint16 *out_type)
{
    return hv_recv_raw_on(hv.in_ring, buf, buflen, out_len, out_type);
}

static void hvpci_process_channel_packets(void)
{
    uint8 buf[2048];
    uint32 len;
    uint16 type;
    int ret;

    if (hvpci.in_ring == NULL)
        return;

    for (;;) {
        memset(buf, 0, sizeof(buf));
        ret = hv_recv_raw_on(hvpci.in_ring, buf, sizeof(buf), &len, &type);
        if (ret != 0 || len == 0)
            break;

        hvpci.protocol_last_packet_type = type;
        hvpci.protocol_last_len = len;
        hvpci_copy_prefix(hvpci.protocol_last_prefix, buf, len);

        if (type == VM_PKT_COMP && len >= sizeof(struct hvpci_response)) {
            struct hvpci_response *rsp = (struct hvpci_response *)buf;
            if (hvpci.protocol_pending &&
                rsp->hdr.trans_id == hvpci.protocol_last_trans_id) {
                hvpci.protocol_last_status = rsp->status;
                __atomic_store_n(&hvpci.protocol_pending, 0,
                                 __ATOMIC_RELEASE);
            }
            if (hvpci.d0_pending &&
                rsp->hdr.trans_id == hvpci.d0_trans_id) {
                hvpci.d0_status = rsp->status;
                hvpci.d0_packet_type = type;
                hvpci.d0_len = len;
                hvpci_copy_prefix(hvpci.d0_prefix, buf, len);
                __atomic_store_n(&hvpci.d0_pending, 0,
                                 __ATOMIC_RELEASE);
            }
        } else if (type == VM_PKT_DATA_INBAND) {
            uint32 msg_type = 0;

            if (len >= sizeof(struct vmpacket_descriptor) +
                sizeof(msg_type))
                memcpy(&msg_type, buf + sizeof(struct vmpacket_descriptor),
                       sizeof(msg_type));
            if (msg_type == HVPCI_BUS_RELATIONS ||
                msg_type == HVPCI_BUS_RELATIONS2) {
                hvpci.query_packet_type = type;
                hvpci.query_len = len;
                hvpci.query_trans_id =
                    ((struct vmpacket_descriptor *)buf)->trans_id;
                hvpci_copy_prefix(hvpci.query_prefix, buf, len);
                (void)hvpci_parse_bus_relations(buf, len);
            }
        }
    }
}

static int hvpci_wait_protocol_response(void)
{
    for (int i = 0; i < HV_WAIT_LOOPS; i++) {
        hv_process_messages();
        if (hvpci.open_ok && hvpci.child_relid != 0 &&
            hv_test_and_clear_event(hvpci.child_relid))
            hvpci_process_channel_packets();
        hvpci_process_channel_packets();
        if (!__atomic_load_n(&hvpci.protocol_pending, __ATOMIC_ACQUIRE))
            return 0;
        sleep_ms(10);
    }
    return -ETIMEDOUT;
}

static int hvpci_negotiate_protocol(void)
{
    static const uint32 versions[] = {
        HVPCI_PROTOCOL_VERSION_1_4,
        HVPCI_PROTOCOL_VERSION_1_3,
        HVPCI_PROTOCOL_VERSION_1_2,
        HVPCI_PROTOCOL_VERSION_1_1,
    };
    struct hvpci_version_request req;
    int ret = -EIO;

    if (!hvpci.open_ok || hvpci.out_ring == NULL)
        return -ENODEV;

    hvpci.protocol_ok = 0;
    hvpci.protocol_selected_version = 0;
    hvpci.protocol_last_status = 0;
    hvpci.protocol_last_ret = 0;

    for (uint32 i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        memset(&req, 0, sizeof(req));
        req.type = HVPCI_QUERY_PROTOCOL_VERSION;
        req.protocol_version = versions[i];

        hvpci.protocol_attempts++;
        hvpci.protocol_last_version = versions[i];
        hvpci.protocol_last_status = (int32)0x80000000U;
        hvpci.protocol_last_trans_id =
            ((uint64)hvpci.protocol_attempts << 32) | versions[i];
        __atomic_store_n(&hvpci.protocol_pending, 1, __ATOMIC_RELEASE);

        ret = hv_send_packet_on(hvpci.out_ring, hvpci.child_relid,
                                hvpci.signal_conn_id,
                                hvpci.monitor_allocated, hvpci.monitorid,
                                hvpci.dedicated, &req, sizeof(req),
                                hvpci.protocol_last_trans_id,
                                VM_PKT_COMPLETION_REQUESTED);
        hvpci.protocol_last_ret = ret;
        if (ret != 0) {
            __atomic_store_n(&hvpci.protocol_pending, 0, __ATOMIC_RELEASE);
            return ret;
        }

        ret = hvpci_wait_protocol_response();
        hvpci.protocol_last_ret = ret;
        if (ret != 0) {
            __atomic_store_n(&hvpci.protocol_pending, 0, __ATOMIC_RELEASE);
            return ret;
        }

        if (hvpci.protocol_last_status >= 0) {
            hvpci.protocol_selected_version = versions[i];
            hvpci.protocol_ok = 1;
            hvpci.protocol_last_ret = 0;
            return 0;
        }
        if ((uint32)hvpci.protocol_last_status !=
                HVPCI_STATUS_REVISION_MISMATCH) {
            hvpci.protocol_last_ret = -EIO;
            return -EIO;
        }
    }

    hvpci.protocol_last_ret = -EIO;
    return -EIO;
}

static int hvpci_wait_d0_response(void)
{
    for (int i = 0; i < HV_WAIT_LOOPS; i++) {
        hv_process_messages();
        if (hvpci.open_ok && hvpci.child_relid != 0 &&
            hv_test_and_clear_event(hvpci.child_relid))
            hvpci_process_channel_packets();
        hvpci_process_channel_packets();
        if (!__atomic_load_n(&hvpci.d0_pending, __ATOMIC_ACQUIRE))
            return 0;
        sleep_ms(10);
    }
    return -ETIMEDOUT;
}

static int hvpci_enter_d0(void)
{
    struct hvpci_bus_d0_entry req;
    int ret;

    hvpci.d0_attempts++;
    hvpci.d0_status = (int32)0x80000000U;
    hvpci.d0_ret = 0;
    hvpci.d0_sent = 0;

    if (!hvpci.open_ok || hvpci.out_ring == NULL)
        return hvpci.d0_ret = -ENODEV;
    if (!hvpci.config_window_ok)
        return hvpci.d0_ret = hvpci.config_window_ret ?
            hvpci.config_window_ret : -ENODEV;

    memset(&req, 0, sizeof(req));
    req.message_type.type = HVPCI_BUS_D0ENTRY;
    req.mmio_base = hvpci.config_window_pa;

    hvpci.d0_trans_id =
        ((uint64)hvpci.d0_attempts << 32) | HVPCI_BUS_D0ENTRY;
    __atomic_store_n(&hvpci.d0_pending, 1, __ATOMIC_RELEASE);
    ret = hv_send_packet_on(hvpci.out_ring, hvpci.child_relid,
                            hvpci.signal_conn_id, hvpci.monitor_allocated,
                            hvpci.monitorid, hvpci.dedicated, &req,
                            sizeof(req), hvpci.d0_trans_id,
                            VM_PKT_COMPLETION_REQUESTED);
    hvpci.d0_ret = ret;
    if (ret != 0) {
        __atomic_store_n(&hvpci.d0_pending, 0, __ATOMIC_RELEASE);
        return ret;
    }
    hvpci.d0_sent++;

    ret = hvpci_wait_d0_response();
    hvpci.d0_ret = ret;
    if (ret != 0) {
        __atomic_store_n(&hvpci.d0_pending, 0, __ATOMIC_RELEASE);
        return ret;
    }
    if (hvpci.d0_status < 0) {
        hvpci.d0_ret = -EIO;
        return -EIO;
    }
    hvpci.d0_ret = 0;
    return 0;
}

static int hvpci_wait_bus_relations(uint32 seen_before)
{
    for (int i = 0; i < HV_WAIT_LOOPS; i++) {
        hv_process_messages();
        if (hvpci.open_ok && hvpci.child_relid != 0 &&
            hv_test_and_clear_event(hvpci.child_relid))
            hvpci_process_channel_packets();
        hvpci_process_channel_packets();
        if (hvpci.relations_seen != seen_before)
            return 0;
        sleep_ms(10);
    }
    return -ETIMEDOUT;
}

static int hvpci_query_bus_relations(void)
{
    struct hvpci_message req;
    uint32 seen_before;
    int ret;

    if (!hvpci.open_ok || hvpci.out_ring == NULL)
        return hvpci.query_ret = -ENODEV;

    memset(&req, 0, sizeof(req));
    req.type = HVPCI_QUERY_BUS_RELATIONS;
    hvpci.query_attempts++;
    hvpci.query_trans_id =
        ((uint64)hvpci.query_attempts << 32) | HVPCI_QUERY_BUS_RELATIONS;
    seen_before = hvpci.relations_seen;
    ret = hv_send_packet_on(hvpci.out_ring, hvpci.child_relid,
                            hvpci.signal_conn_id, hvpci.monitor_allocated,
                            hvpci.monitorid, hvpci.dedicated, &req,
                            sizeof(req), hvpci.query_trans_id, 0);
    hvpci.query_ret = ret;
    if (ret != 0)
        return ret;
    hvpci.query_sent++;

    ret = hvpci_wait_bus_relations(seen_before);
    hvpci.query_ret = ret;
    return ret;
}

static int hv_establish_gpadl_large(uint32 child_relid, uint32 gpadl,
                                    uint64 base_pa, uint32 byte_count,
                                    volatile int *device_ok,
                                    uint32 *device_status)
{
    struct vmbus_gpadl_header_large hdr;
    struct vmbus_gpadl_body body;
    uint32 page_count;
    uint32 sent;
    uint32 msgno = 0;

    if (child_relid == 0 || byte_count == 0)
        return -EINVAL;
    page_count = (byte_count + PGSIZE - 1) >> PGSHIFT;
    if (page_count == 0)
        return -EINVAL;

    memset(&hdr, 0, sizeof(hdr));
    hdr.header.msgtype = CHANNELMSG_GPADL_HEADER;
    hdr.child_relid = child_relid;
    hdr.gpadl = gpadl;
    hdr.range_buflen = 8 + page_count * sizeof(uint64);
    hdr.rangecount = 1;
    hdr.byte_count = byte_count;
    hdr.byte_offset = base_pa & (PGSIZE - 1);

    uint32 first = page_count < HV_GPADL_HEADER_MAX_PFNS ?
        page_count : HV_GPADL_HEADER_MAX_PFNS;
    uint64 page_base = base_pa & ~(uint64)(PGSIZE - 1);
    for (uint32 i = 0; i < first; i++)
        hdr.pfn[i] = (page_base + (uint64)i * PGSIZE) >> PGSHIFT;

    hv_gpadl_wait_handle = gpadl;
    hv_gpadl_wait_status = 0xffffffffU;
    __atomic_store_n(&hv_gpadl_wait_ok, 0, __ATOMIC_RELEASE);
    if (device_ok)
        __atomic_store_n(device_ok, 0, __ATOMIC_RELEASE);
    if (device_status)
        *device_status = 0xffffffffU;

    uint32 hdr_len = sizeof(hdr) -
        (HV_GPADL_HEADER_MAX_PFNS - first) * sizeof(uint64);
    if (hv_post_msg(hv.msg_conn_id, &hdr, hdr_len) != 0)
        return -EIO;

    sent = first;
    while (sent < page_count) {
        uint32 n = page_count - sent;
        if (n > HV_GPADL_BODY_MAX_PFNS)
            n = HV_GPADL_BODY_MAX_PFNS;
        memset(&body, 0, sizeof(body));
        body.header.msgtype = CHANNELMSG_GPADL_BODY;
        body.msgnumber = msgno++;
        body.gpadl = gpadl;
        for (uint32 i = 0; i < n; i++)
            body.pfn[i] = (page_base + (uint64)(sent + i) * PGSIZE) >>
                          PGSHIFT;
        uint32 body_len = sizeof(body) -
            (HV_GPADL_BODY_MAX_PFNS - n) * sizeof(uint64);
        if (hv_post_msg(hv.msg_conn_id, &body, body_len) != 0)
            return -EIO;
        sent += n;
    }

    int ret = hv_wait_flag(&hv_gpadl_wait_ok);
    if (device_status)
        *device_status = hv_gpadl_wait_status;
    if (device_ok)
        __atomic_store_n(device_ok,
                         hv_gpadl_wait_status == 0,
                         __ATOMIC_RELEASE);
    if (ret != 0)
        return ret;
    return hv_gpadl_wait_status == 0 ? 0 : -EIO;
}

static int hv_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hv.child_relid, HV_GPADL_HANDLE,
                                    hv.ring_pa, HV_RING_PAGES * PGSIZE,
                                    &hv.gpadl_ok, &hv.gpadl_status);
}

static int hvstor_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvstor.child_relid, HV_STOR_GPADL_HANDLE,
                                    hvstor.ring_pa, HV_RING_PAGES * PGSIZE,
                                    &hvstor.gpadl_ok,
                                    &hvstor.gpadl_status);
}

static int hvnet_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvnet.child_relid, HV_NET_GPADL_HANDLE,
                                    hvnet.ring_pa, HV_RING_PAGES * PGSIZE,
                                    &hvnet.gpadl_ok,
                                    &hvnet.gpadl_status);
}

static int hvnet_establish_recv_gpadl(void)
{
    return hv_establish_gpadl_large(hvnet.child_relid,
                                    HV_NET_RECV_GPADL_HANDLE,
                                    hvnet.recv_buf_pa,
                                    hvnet.recv_buf_size,
                                    &hvnet.recv_gpadl_ok,
                                    &hvnet.recv_gpadl_status);
}

static int hvnet_establish_send_gpadl(void)
{
    return hv_establish_gpadl_large(hvnet.child_relid,
                                    HV_NET_SEND_GPADL_HANDLE,
                                    hvnet.send_buf_pa,
                                    hvnet.send_buf_size,
                                    &hvnet.send_gpadl_ok,
                                    &hvnet.send_gpadl_status);
}

static int hv_open_channel(void)
{
    struct vmbus_open_channel msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.msgtype = CHANNELMSG_OPENCHANNEL;
    msg.child_relid = hv.child_relid;
    msg.openid = hv.child_relid;
    msg.ringbuffer_gpadlhandle = HV_GPADL_HANDLE;
    msg.target_vp = 0;
    msg.downstream_ringbuffer_pageoffset = HV_SEND_PAGES;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(&hv.open_ok);
}

static int hvstor_open_channel(void)
{
    struct vmbus_open_channel msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.msgtype = CHANNELMSG_OPENCHANNEL;
    msg.child_relid = hvstor.child_relid;
    msg.openid = hvstor.child_relid;
    msg.ringbuffer_gpadlhandle = HV_STOR_GPADL_HANDLE;
    msg.target_vp = 0;
    msg.downstream_ringbuffer_pageoffset = HV_SEND_PAGES;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(&hvstor.open_ok);
}

static int hvnet_open_channel(void)
{
    struct vmbus_open_channel msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.msgtype = CHANNELMSG_OPENCHANNEL;
    msg.child_relid = hvnet.child_relid;
    msg.openid = hvnet.child_relid;
    msg.ringbuffer_gpadlhandle = HV_NET_GPADL_HANDLE;
    msg.target_vp = 0;
    msg.downstream_ringbuffer_pageoffset = HV_SEND_PAGES;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(&hvnet.open_ok);
}

static int hvkbd_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvkbd.child_relid, HV_KBD_GPADL_HANDLE,
                                    hvkbd.ring_pa, HV_RING_PAGES * PGSIZE,
                                    &hvkbd.gpadl_ok,
                                    &hvkbd.gpadl_status);
}

static int hvkbd_open_channel(void)
{
    struct vmbus_open_channel msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.msgtype = CHANNELMSG_OPENCHANNEL;
    msg.child_relid = hvkbd.child_relid;
    msg.openid = hvkbd.child_relid;
    msg.ringbuffer_gpadlhandle = HV_KBD_GPADL_HANDLE;
    msg.target_vp = 0;
    msg.downstream_ringbuffer_pageoffset = HV_SEND_PAGES;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(&hvkbd.open_ok);
}

static int hid_read_sbits(const uint8 *report, uint32 report_len,
                          uint16 bit, uint8 size)
{
    uint32 v = 0;
    for (uint8 i = 0; i < size && i < 31; i++) {
        uint32 b = bit + i;
        if ((b >> 3) < report_len && (report[b >> 3] & (1U << (b & 7))))
            v |= 1U << i;
    }
    if (size > 0 && size < 31 && (v & (1U << (size - 1))))
        v |= ~((1U << size) - 1);
    return (int)v;
}

static uint32 hid_read_ubits(const uint8 *report, uint32 report_len,
                             uint16 bit, uint8 size)
{
    uint32 v = 0;
    for (uint8 i = 0; i < size && i < 31; i++) {
        uint32 b = bit + i;
        if ((b >> 3) < report_len && (report[b >> 3] & (1U << (b & 7))))
            v |= 1U << i;
    }
    return v;
}

static uint16 hid_scale_absolute(uint32 value, int logical_min,
                                 int logical_max)
{
    if (logical_max <= logical_min)
        return (uint16)value;
    if (value < (uint32)logical_min)
        value = (uint32)logical_min;
    if (value > (uint32)logical_max)
        value = (uint32)logical_max;

    uint64 span = (uint64)((uint32)logical_max - (uint32)logical_min);
    uint64 pos = (uint64)(value - (uint32)logical_min);
    return (uint16)((pos * 65535u) / span);
}

static void hid_set_field(struct hv_hid_field *f, uint16 bit, uint8 size,
                          int logical_min, int logical_max, int relative)
{
    f->valid = 1;
    f->bit = bit;
    f->size = size;
    f->logical_min = logical_min;
    f->logical_max = logical_max;
    f->relative = relative;
}

static int hid_item_value(const uint8 *p, uint8 size)
{
    int v = 0;
    if (size == 1)
        v = (int8)p[0];
    else if (size == 2)
        v = (int16)(p[0] | (p[1] << 8));
    else if (size == 4)
        v = (int)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    return v;
}

static void hid_parse_report_desc(const uint8 *desc, uint32 len)
{
    uint16 usage_page = 0, usage_min = 0, usage_max = 0;
    uint16 usages[16];
    uint8 usage_count = 0, report_size = 0, report_count = 0;
    int logical_min = 0, logical_max = 0;

    memset(&hv.hid, 0, sizeof(hv.hid));
    for (uint32 i = 0; i < len;) {
        uint8 b = desc[i++];
        if (b == 0xfe) {
            if (i + 1 >= len)
                break;
            uint8 n = desc[i];
            i += 2 + n;
            continue;
        }
        uint8 sz_code = b & 3;
        uint8 size = sz_code == 3 ? 4 : sz_code;
        uint8 type = (b >> 2) & 3;
        uint8 tag = (b >> 4) & 0xf;
        if (i + size > len)
            break;
        int value = hid_item_value(&desc[i], size);
        uint32 uvalue = (uint32)value;
        i += size;

        if (type == 1) {
            if (tag == 0)
                usage_page = (uint16)uvalue;
            else if (tag == 1)
                logical_min = value;
            else if (tag == 2)
                logical_max = value;
            else if (tag == 7)
                report_size = (uint8)uvalue;
            else if (tag == 8) {
                hv.hid.report_id = (uint8)uvalue;
                hv.hid.bitpos = 8;
            } else if (tag == 9)
                report_count = (uint8)uvalue;
        } else if (type == 2) {
            if (tag == 0 && usage_count < 16)
                usages[usage_count++] = (uint16)uvalue;
            else if (tag == 1)
                usage_min = (uint16)uvalue;
            else if (tag == 2)
                usage_max = (uint16)uvalue;
        } else if (type == 0 && tag == 8) {
            int relative = (uvalue & 0x04) != 0;
            for (uint8 n = 0; n < report_count; n++) {
                uint16 usage = n < usage_count ? usages[n] :
                    (usage_min ? (uint16)(usage_min + n) : 0);
                if (usage_max && usage > usage_max)
                    usage = 0;
                if (usage_page == 0x01 && usage == 0x30)
                    hid_set_field(&hv.hid.x, hv.hid.bitpos, report_size,
                                  logical_min, logical_max, relative);
                else if (usage_page == 0x01 && usage == 0x31)
                    hid_set_field(&hv.hid.y, hv.hid.bitpos, report_size,
                                  logical_min, logical_max, relative);
                else if (usage_page == 0x01 && usage == 0x38)
                    hid_set_field(&hv.hid.wheel, hv.hid.bitpos, report_size,
                                  logical_min, logical_max, relative);
                else if (usage_page == 0x09 && usage >= 1 && usage <= 3)
                    hid_set_field(&hv.hid.buttons[usage - 1], hv.hid.bitpos,
                                  report_size, logical_min, logical_max, 0);
                hv.hid.bitpos += report_size;
            }
            usage_count = 0;
            usage_min = usage_max = 0;
        } else if (type == 0) {
            usage_count = 0;
            usage_min = usage_max = 0;
        }
    }
}

static void hv_emit_report(const uint8 *report, uint32 len)
{
    if (hv.hid.report_id) {
        if (len == 0 || report[0] != hv.hid.report_id) {
            if (hv.ignored_reports++ < 4)
                printf("hyperv-input: ignored report id len=%u first=0x%x expected=%u\n",
                       len, len ? report[0] : 0, hv.hid.report_id);
            return;
        }
    }
    if (!hv.hid.x.valid || !hv.hid.y.valid) {
        if (hv.ignored_reports++ < 4)
            printf("hyperv-input: ignored report without x/y fields len=%u\n", len);
        return;
    }

    struct mouse_event ev;
    memset(&ev, 0, sizeof(ev));

    for (int i = 0; i < 3; i++) {
        if (hv.hid.buttons[i].valid &&
            hid_read_ubits(report, len, hv.hid.buttons[i].bit,
                           hv.hid.buttons[i].size))
            ev.buttons |= 1U << i;
    }

    int x = hid_read_sbits(report, len, hv.hid.x.bit, hv.hid.x.size);
    int y = hid_read_sbits(report, len, hv.hid.y.bit, hv.hid.y.size);
    int wheel = hv.hid.wheel.valid ?
        hid_read_sbits(report, len, hv.hid.wheel.bit, hv.hid.wheel.size) : 0;

    if (!hv.hid.x.relative && !hv.hid.y.relative &&
        hv.hid.x.logical_max > hv.hid.x.logical_min &&
        hv.hid.y.logical_max > hv.hid.y.logical_min) {
        uint32 ux = hid_read_ubits(report, len, hv.hid.x.bit, hv.hid.x.size);
        uint32 uy = hid_read_ubits(report, len, hv.hid.y.bit, hv.hid.y.size);
        ev.flags = MOUSE_EVENT_F_ABSOLUTE;
        ev.dx = (int16)hid_scale_absolute(ux, hv.hid.x.logical_min,
                                          hv.hid.x.logical_max);
        ev.dy = (int16)hid_scale_absolute(uy, hv.hid.y.logical_min,
                                          hv.hid.y.logical_max);
    } else {
        ev.dx = (int16)x;
        ev.dy = (int16)y;
    }
    ev.dz = (int8)wheel;
    mouse_input_push_event(&ev);
    hv.reports++;
    if (hv_debug_enabled() && hv.reports <= 8) {
        printf("hyperv-input: report #%lu len=%u flags=0x%x dx=%d dy=%d btn=0x%x dz=%d raw=",
               hv.reports, len, ev.flags, ev.dx, ev.dy, ev.buttons, ev.dz);
        for (uint32 i = 0; i < len && i < 8; i++)
            printf("%02x", report[i]);
        printf("\n");
    }
}

static void hv_handle_pipe_payload(const uint8 *payload, uint32 len)
{
    if (len < sizeof(struct pipe_msg))
        return;
    const struct pipe_msg *pipe = (const struct pipe_msg *)payload;
    if (pipe->type != PIPE_MESSAGE_DATA || pipe->size + sizeof(*pipe) > len)
        return;
    if (pipe->size < sizeof(struct synthhid_msg_hdr))
        return;

    const struct synthhid_msg_hdr *hdr =
        (const struct synthhid_msg_hdr *)pipe->data;
    switch (hdr->type) {
    case SYNTH_HID_PROTOCOL_RESPONSE: {
        const struct synthhid_protocol_response *r =
            (const struct synthhid_protocol_response *)hdr;
        if (pipe->size >= sizeof(*r) && r->approved)
            hv.protocol_ok = 1;
        break;
    }
    case SYNTH_HID_INITIAL_DEVICE_INFO: {
        if (pipe->size < 8 + 32 + 9)
            break;
        const uint8 *hid_desc = pipe->data + 8 + 32;
        uint8 desc_len = hid_desc[0];
        uint16 report_len = hid_desc[7] | (hid_desc[8] << 8);
        if (desc_len && 8 + 32 + desc_len + report_len <= pipe->size) {
            const uint8 *report_desc = hid_desc + desc_len;
            hid_parse_report_desc(report_desc, report_len);
            printf("hyperv-input: hid report desc=%u id=%u x=%d y=%d wheel=%d\n",
                   report_len, hv.hid.report_id, hv.hid.x.valid,
                   hv.hid.y.valid, hv.hid.wheel.valid);
            printf("hyperv-input: fields x bit=%u size=%u min=%d max=%d rel=%d; y bit=%u size=%u min=%d max=%d rel=%d\n",
                   hv.hid.x.bit, hv.hid.x.size, hv.hid.x.logical_min,
                   hv.hid.x.logical_max, hv.hid.x.relative, hv.hid.y.bit,
                   hv.hid.y.size, hv.hid.y.logical_min, hv.hid.y.logical_max,
                   hv.hid.y.relative);
        }

        struct mousevsc_msg ack;
        memset(&ack, 0, sizeof(ack));
        ack.type = PIPE_MESSAGE_DATA;
        ack.size = sizeof(struct synthhid_device_info_ack);
        ack.ack.header.type = SYNTH_HID_INITIAL_DEVICE_INFO_ACK;
        ack.ack.header.size = 1;
        hv_send_packet(&ack, 8 + sizeof(struct synthhid_device_info_ack),
                       (uint64)&ack, VM_PKT_COMPLETION_REQUESTED);
        hv.device_info_ok = 1;
        break;
    }
    case SYNTH_HID_INPUT_REPORT: {
        const struct synthhid_input_report *r =
            (const struct synthhid_input_report *)hdr;
        hv.input_packets++;
        if (pipe->size >= sizeof(*r) && r->header.size <= pipe->size - 8)
            hv_emit_report(r->buffer, r->header.size);
        else if (hv.ignored_reports++ < 4)
            printf("hyperv-input: bad input packet pipe=%u hdr=%u\n",
                   pipe->size, r->header.size);
        break;
    }
    default:
        break;
    }
}

static void hv_process_channel_packets(void)
{
    uint8 pkt[512];
    uint32 len;
    uint16 type;

    for (int i = 0; i < 32; i++) {
        if (hv_recv_raw(pkt, sizeof(pkt), &len, &type) != 0 || len == 0)
            return;
        if (type == VM_PKT_DATA_INBAND) {
            struct vmpacket_descriptor *desc = (struct vmpacket_descriptor *)pkt;
            uint32 off = desc->offset8 << 3;
            uint32 plen = (desc->len8 << 3);
            if (off <= plen && plen <= len)
                hv_handle_pipe_payload(pkt + off, plen - off);
        } else if (type == VM_PKT_COMP) {
            continue;
        }
    }
}

static void hvkbd_handle_payload(const uint8 *payload, uint32 len)
{
    if (len < sizeof(uint32))
        return;

    uint32 type = *(const uint32 *)payload;
    switch (type) {
    case SYNTH_KBD_PROTOCOL_RESPONSE: {
        const struct synthkbd_protocol_response *r =
            (const struct synthkbd_protocol_response *)payload;
        if (len >= sizeof(*r) && (r->status & SYNTH_KBD_STATUS_ACCEPTED))
            hvkbd.protocol_ok = 1;
        break;
    }
    case SYNTH_KBD_EVENT: {
        const struct synthkbd_keystroke *k =
            (const struct synthkbd_keystroke *)payload;
        if (len < sizeof(*k))
            break;

        if (k->info & SYNTH_KBD_IS_E0)
            ps2kbd_handle_byte(0xe0);
        if (k->info & SYNTH_KBD_IS_E1)
            ps2kbd_handle_byte(0xe1);

        uint8 sc = (uint8)(k->make_code & 0x7f);
        if (k->info & SYNTH_KBD_IS_BREAK)
            sc |= 0x80;
        ps2kbd_handle_byte(sc);
        hvkbd.events++;
        if (hvkbd.events <= 8)
            printf("hyperv-input: kbd event #%lu scan=0x%x info=0x%x\n",
                   hvkbd.events, k->make_code, k->info);
        break;
    }
    default:
        break;
    }
}

static void hvkbd_process_channel_packets(void)
{
    uint8 pkt[512];
    uint32 len;
    uint16 type;

    for (int i = 0; i < 32; i++) {
        if (hv_recv_raw_on(hvkbd.in_ring, pkt, sizeof(pkt), &len, &type) != 0 ||
            len == 0)
            return;
        if (type == VM_PKT_DATA_INBAND) {
            struct vmpacket_descriptor *desc = (struct vmpacket_descriptor *)pkt;
            uint32 off = desc->offset8 << 3;
            uint32 plen = desc->len8 << 3;
            if (off <= plen && plen <= len)
                hvkbd_handle_payload(pkt + off, plen - off);
        } else if (type == VM_PKT_COMP) {
            continue;
        }
    }
}

static uint32 be32_get(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) | (uint32)p[3];
}

static void be32_put(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

static void be16_put(uint8 *p, uint16 v)
{
    p[0] = (uint8)(v >> 8);
    p[1] = (uint8)v;
}

static void hvstor_complete(const struct vstor_packet *rsp, uint64 trans_id)
{
    uint64 waiting =
        __atomic_load_n(&hvstor.waiting_trans_id, __ATOMIC_ACQUIRE);
    if (waiting == 0 || trans_id != waiting) {
        uint64 stale =
            __atomic_add_fetch(&hvstor.stale_completions, 1,
                               __ATOMIC_RELAXED);
        if (stale <= 8)
            printf("hyperv-storvsc: ignoring stale completion trans=%lx waiting=%lx\n",
                   trans_id, waiting);
        return;
    }
    hvstor.completion = *rsp;
    hvstor.completion_trans_id = trans_id;
    __atomic_store_n(&hvstor.completion_pending, 1, __ATOMIC_RELEASE);
}

static uint64 hvstor_next_trans_id(void)
{
    return __atomic_add_fetch(&hvstor.next_trans_id, 1, __ATOMIC_RELAXED);
}

static void hvstor_process_channel_packets(void)
{
    uint8 pkt[768];
    uint32 len;
    uint16 type;

    for (int i = 0; i < 64; i++) {
        if (hv_recv_raw_on(hvstor.in_ring, pkt, sizeof(pkt), &len, &type) != 0 ||
            len == 0)
            return;
        if (type != VM_PKT_DATA_INBAND && type != VM_PKT_COMP)
            continue;
        struct vmpacket_descriptor *desc = (struct vmpacket_descriptor *)pkt;
        uint32 off = desc->offset8 << 3;
        uint32 plen = desc->len8 << 3;
        if (off > plen || plen > len ||
            plen - off < sizeof(struct vstor_packet))
            continue;
        const struct vstor_packet *rsp =
            (const struct vstor_packet *)(pkt + off);
        if (rsp->operation == VSTOR_OPERATION_COMPLETE_IO ||
            rsp->operation == VSTOR_OPERATION_BEGIN_INITIALIZATION ||
            rsp->operation == VSTOR_OPERATION_QUERY_PROTOCOL ||
            rsp->operation == VSTOR_OPERATION_QUERY_PROPERTIES ||
            rsp->operation == VSTOR_OPERATION_END_INITIALIZATION)
            hvstor_complete(rsp, desc->trans_id);
    }
}

extern uint64 g_net_tx_packets;
extern uint64 g_net_tx_bytes;
extern uint64 g_net_rx_packets;
extern uint64 g_net_rx_bytes;

static uint32 hvnet_nvsp_len(const struct nvsp_message *msg)
{
    uint32 len;
    switch (msg->hdr.msg_type) {
    case NVSP_MSG_TYPE_INIT:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_message_init);
        break;
    case NVSP_MSG_TYPE_INIT_COMPLETE:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_message_init_complete);
        break;
    case NVSP_MSG1_TYPE_SEND_NDIS_VER:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_ndis_version);
        break;
    case NVSP_MSG1_TYPE_SEND_RECV_BUF:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_receive_buffer);
        break;
    case NVSP_MSG1_TYPE_SEND_RECV_BUF_COMPLETE:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_receive_buffer_complete);
        break;
    case NVSP_MSG1_TYPE_SEND_SEND_BUF:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_send_buffer);
        break;
    case NVSP_MSG1_TYPE_SEND_SEND_BUF_COMPLETE:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_send_buffer_complete);
        break;
    case NVSP_MSG1_TYPE_SEND_RNDIS_PKT:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_rndis_packet);
        break;
    case NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_1_message_send_rndis_packet_complete);
        break;
    case NVSP_MSG2_TYPE_SEND_NDIS_CONFIG:
        len = sizeof(struct nvsp_message_header) +
              sizeof(struct nvsp_2_send_ndis_config);
        break;
    default:
        len = sizeof(*msg);
        break;
    }
    return len < NVSP_MESSAGE_WIRE_SIZE ? NVSP_MESSAGE_WIRE_SIZE : len;
}

static int hvnet_send_nvsp(struct nvsp_message *msg, uint32 flags)
{
    uint32 len = hvnet_nvsp_len(msg);
    if (hv_debug_enabled() && hvnet.debug_tx_count < 32) {
        printf("hyperv-netvsc: tx nvsp type=%u flags=0x%x len=%u\n",
               msg->hdr.msg_type, flags, len);
        hvnet.debug_tx_count++;
    }
    return hv_send_packet_on(hvnet.out_ring, hvnet.child_relid,
                             hvnet.signal_conn_id, hvnet.monitor_allocated,
                             hvnet.monitorid, hvnet.dedicated,
                             msg, len, (uint64)msg, flags);
}

static int hvnet_tx_section_from_trans(uint64 trans_id, uint32 *idx);
static void hvnet_free_send_section(uint32 section);

static void hvnet_complete_nvsp(const struct nvsp_message *msg,
                                uint64 trans_id)
{
    switch (msg->hdr.msg_type) {
    case NVSP_MSG_TYPE_INIT_COMPLETE:
    case NVSP_MSG1_TYPE_SEND_RECV_BUF_COMPLETE:
    case NVSP_MSG1_TYPE_SEND_SEND_BUF_COMPLETE:
        hvnet.response = *msg;
        __atomic_store_n(&hvnet.response_pending, 1, __ATOMIC_RELEASE);
        break;
    case NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE:
        if (hvnet_tx_section_from_trans(trans_id, NULL)) {
            if (hv_debug_enabled() &&
                msg->msg.v1.send_rndis_pkt_complete.status !=
                NVSP_STAT_SUCCESS && hvnet.debug_tx_count < 64) {
                printf("hyperv-netvsc: tx section complete trans=%lx status=%u\n",
                       trans_id,
                       msg->msg.v1.send_rndis_pkt_complete.status);
                hvnet.debug_tx_count++;
            }
            uint32 section;
            if (hvnet_tx_section_from_trans(trans_id, &section))
                hvnet_free_send_section(section);
        } else {
            hvnet.rndis_send_status =
                msg->msg.v1.send_rndis_pkt_complete.status;
            hvnet.rndis_send_trans_id = trans_id;
            __atomic_store_n(&hvnet.rndis_send_done, 1, __ATOMIC_RELEASE);
        }
        break;
    default:
        break;
    }
}

static int hvnet_wait_nvsp(uint32 msg_type, struct nvsp_message *out)
{
    for (int i = 0; i < HV_NET_WAIT_LOOPS; i++) {
        hv_process_messages();
        hv_process_events();
        if (hvnet.open_ok)
            hvnet_process_channel_packets();
        if (__atomic_load_n(&hvnet.response_pending, __ATOMIC_ACQUIRE) &&
            hvnet.response.hdr.msg_type == msg_type) {
            if (out)
                *out = hvnet.response;
            __atomic_store_n(&hvnet.response_pending, 0,
                             __ATOMIC_RELEASE);
            return 0;
        }
        sleep_ms(1);
    }
    printf("hyperv-netvsc: timeout waiting nvsp type=%u last=%u\n",
           msg_type, hvnet.response.hdr.msg_type);
    if (hvnet.out_ring)
        printf("hyperv-netvsc: outbound ring read=%u write=%u mask=%u pending=%u monitor=%d relid=%u conn=%u monid=%u\n",
               hvnet.out_ring->read_index, hvnet.out_ring->write_index,
               hvnet.out_ring->interrupt_mask,
               hvnet.out_ring->pending_send_sz,
               hvnet.monitor_allocated, hvnet.child_relid,
               hvnet.signal_conn_id, hvnet.monitorid);
    hv_clear_channel_signal(hvnet.child_relid, hvnet.monitor_allocated,
                            hvnet.monitorid);
    return -ETIMEDOUT;
}

static int hvnet_wait_rndis_send(uint64 trans_id)
{
    for (int i = 0; i < HV_NET_WAIT_LOOPS; i++) {
        hv_process_messages();
        hv_process_events();
        if (hvnet.open_ok)
            hvnet_process_channel_packets();
        if (__atomic_load_n(&hvnet.rndis_send_done, __ATOMIC_ACQUIRE) &&
            hvnet.rndis_send_trans_id == trans_id) {
            __atomic_store_n(&hvnet.rndis_send_done, 0, __ATOMIC_RELEASE);
            return hvnet.rndis_send_status == NVSP_STAT_SUCCESS ? 0 : -EIO;
        }
        sleep_ms(1);
    }
    printf("hyperv-netvsc: timeout waiting rndis send trans=%lx\n",
           trans_id);
    if (hvnet.out_ring)
        printf("hyperv-netvsc: rndis send ring read=%u write=%u mask=%u pending=%u\n",
               hvnet.out_ring->read_index, hvnet.out_ring->write_index,
               hvnet.out_ring->interrupt_mask,
               hvnet.out_ring->pending_send_sz);
    return -ETIMEDOUT;
}

static int hvnet_tx_section_from_trans(uint64 trans_id, uint32 *idx)
{
    uint32 section;

    if ((trans_id & HV_NET_TX_SECTION_TRANS_MASK) !=
        HV_NET_TX_SECTION_TRANS_BASE)
        return 0;
    section = (uint32)(trans_id & 0xffffffffU);
    if (section >= hvnet.send_section_count)
        return 0;
    if (idx)
        *idx = section;
    return 1;
}

static void hvnet_free_send_section(uint32 section)
{
    if (section >= hvnet.send_section_count)
        return;
    __atomic_store_n(&hvnet.send_section_busy[section], 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&hvnet.tx_inflight, __ATOMIC_ACQUIRE) > 0)
        __atomic_fetch_sub(&hvnet.tx_inflight, 1, __ATOMIC_ACQ_REL);
}

static int hvnet_alloc_send_section(void)
{
    uint32 count = hvnet.send_section_count;

    for (uint32 i = 0; i < count; i++) {
        uint8 expected = 0;

        if (__atomic_compare_exchange_n(&hvnet.send_section_busy[i],
                                        &expected, 1, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&hvnet.tx_inflight, 1, __ATOMIC_ACQ_REL);
            return (int)i;
        }
    }
    return -EAGAIN;
}

static int hvnet_wait_rndis_response(uint32 req_id, uint32 msg_type,
                                     uint8 *out, uint32 *out_len)
{
    for (int i = 0; i < HV_NET_WAIT_LOOPS; i++) {
        hv_process_messages();
        hv_process_events();
        if (hvnet.open_ok)
            hvnet_process_channel_packets();
        if (__atomic_load_n(&hvnet.rndis_response_pending,
                            __ATOMIC_ACQUIRE) &&
            hvnet.rndis_response_req_id == req_id &&
            hvnet.rndis_response_type == msg_type) {
            if (out && out_len) {
                uint32 n = hvnet.rndis_response_len;
                if (n > *out_len)
                    n = *out_len;
                memcpy(out, hvnet.rndis_response, n);
                *out_len = n;
            }
            __atomic_store_n(&hvnet.rndis_response_pending, 0,
                             __ATOMIC_RELEASE);
            return 0;
        }
        sleep_ms(1);
    }
    printf("hyperv-netvsc: timeout waiting rndis req=%u type=0x%x last req=%u type=0x%x\n",
           req_id, msg_type, hvnet.rndis_response_req_id,
           hvnet.rndis_response_type);
    return -ETIMEDOUT;
}

static int hvnet_send_rndis_section(uint32 section, uint32 len)
{
    struct nvsp_message nvmsg;
    uint64 trans_id;
    int ret;

    if (section >= hvnet.send_section_count || len == 0 ||
        len > hvnet.send_section_size)
        return -EINVAL;

    memset(&nvmsg, 0, sizeof(nvmsg));
    nvmsg.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RNDIS_PKT;
    nvmsg.msg.v1.send_rndis_pkt.channel_type = 0;
    nvmsg.msg.v1.send_rndis_pkt.send_buf_section_index = section;
    nvmsg.msg.v1.send_rndis_pkt.send_buf_section_size = len;
    trans_id = HV_NET_TX_SECTION_TRANS_BASE | section;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ret = hv_send_packet_on(hvnet.out_ring, hvnet.child_relid,
                            hvnet.signal_conn_id, hvnet.monitor_allocated,
                            hvnet.monitorid, hvnet.dedicated,
                            &nvmsg, hvnet_nvsp_len(&nvmsg), trans_id,
                            VM_PKT_COMPLETION_REQUESTED);
    if (ret != 0)
        hvnet_free_send_section(section);
    return ret;
}

static int hvnet_send_rndis_raw(void *buf, uint32 len, int wait_send,
                                uint32 channel_type)
{
    struct nvsp_message nvmsg;
    struct hv_multipage_buffer mpb;
    uint64 pa = (uint64)buf;
    uint64 trans_id = pa;
    uint32 page_count;

    if (len == 0)
        return -EINVAL;
    memset(&nvmsg, 0, sizeof(nvmsg));
    nvmsg.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RNDIS_PKT;
    nvmsg.msg.v1.send_rndis_pkt.channel_type = channel_type;

    if (channel_type == 0 &&
        hvnet.send_buf != NULL && hvnet.send_section_size != 0 &&
        len <= hvnet.send_section_size) {
        memcpy(hvnet.send_buf, buf, len);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        nvmsg.msg.v1.send_rndis_pkt.send_buf_section_index = 0;
        nvmsg.msg.v1.send_rndis_pkt.send_buf_section_size = len;

        __atomic_store_n(&hvnet.rndis_send_done, 0, __ATOMIC_RELEASE);
        int ret = hv_send_packet_on(hvnet.out_ring, hvnet.child_relid,
                                    hvnet.signal_conn_id,
                                    hvnet.monitor_allocated,
                                    hvnet.monitorid, hvnet.dedicated,
                                    &nvmsg, hvnet_nvsp_len(&nvmsg),
                                    hvnet.send_buf_pa,
                                    VM_PKT_COMPLETION_REQUESTED);
        if (ret != 0)
            return ret;
        if (!wait_send)
            return 0;
        return hvnet_wait_rndis_send(hvnet.send_buf_pa);
    }

    nvmsg.msg.v1.send_rndis_pkt.send_buf_section_index =
        NETVSC_INVALID_INDEX;
    nvmsg.msg.v1.send_rndis_pkt.send_buf_section_size = 0;

    memset(&mpb, 0, sizeof(mpb));
    mpb.len = len;
    mpb.offset = pa & (PGSIZE - 1);
    page_count = (mpb.offset + len + PGSIZE - 1) >> PGSHIFT;
    if (page_count > HV_STOR_MAX_PAGES)
        return -EINVAL;
    uint64 base = pa & ~(uint64)(PGSIZE - 1);
    for (uint32 i = 0; i < page_count; i++)
        mpb.pfn_array[i] = (base + (uint64)i * PGSIZE) >> PGSHIFT;

    __atomic_store_n(&hvnet.rndis_send_done, 0, __ATOMIC_RELEASE);
    int ret = hv_send_packet_mpb_on(hvnet.out_ring, hvnet.child_relid,
                                    hvnet.signal_conn_id,
                                    hvnet.monitor_allocated,
                                    hvnet.monitorid, hvnet.dedicated,
                                    &mpb, &nvmsg,
                                    hvnet_nvsp_len(&nvmsg), trans_id);
    if (ret != 0)
        return ret;
    if (!wait_send)
        return 0;
    return hvnet_wait_rndis_send(trans_id);
}

static uint32 rndis_next_req_id(void)
{
    uint32 id = ++hvnet.rndis_req_id;
    if (id == 0)
        id = ++hvnet.rndis_req_id;
    return id;
}

static int rndis_send_control(uint8 *req, uint32 len, uint32 req_id,
                              uint32 complete_type, uint8 *rsp,
                              uint32 *rsp_len)
{
    uint8 *page = (uint8 *)page_alloc(0, PAGE_TYPE_ANON);
    if (page == NULL)
        return -ENOMEM;
    memset(page, 0, PGSIZE);
    memcpy(page, req, len);

    __atomic_store_n(&hvnet.rndis_response_pending, 0, __ATOMIC_RELEASE);
    int ret = hvnet_send_rndis_raw(page, len, 1, 1);
    if (ret != 0) {
        page_free(page, 0);
        return ret;
    }
    ret = hvnet_wait_rndis_response(req_id, complete_type, rsp, rsp_len);
    page_free(page, 0);
    return ret;
}

static int rndis_init_device(void)
{
    uint8 req[128], rsp[256];
    struct rndis_message_header *mh = (struct rndis_message_header *)req;
    struct rndis_initialize_request *init =
        (struct rndis_initialize_request *)(req + sizeof(*mh));
    uint32 req_id = rndis_next_req_id();
    uint32 rsp_len = sizeof(rsp);

    memset(req, 0, sizeof(req));
    mh->msg_type = RNDIS_MSG_INIT;
    mh->msg_len = sizeof(*mh) + sizeof(*init);
    init->req_id = req_id;
    init->major_ver = RNDIS_MAJOR_VERSION;
    init->minor_ver = RNDIS_MINOR_VERSION;
    init->max_xfer_size = 0x4000;

    int ret = rndis_send_control(req, mh->msg_len, req_id,
                                 RNDIS_MSG_INIT_C, rsp, &rsp_len);
    if (ret != 0 || rsp_len < sizeof(*mh) + sizeof(struct rndis_initialize_complete))
        return ret != 0 ? ret : -EIO;
    struct rndis_initialize_complete *c =
        (struct rndis_initialize_complete *)(rsp + sizeof(*mh));
    if (c->status != RNDIS_STATUS_SUCCESS)
        return -EIO;
    printf("hyperv-netvsc: RNDIS init max_pkt=%u max_xfer=%u align=%u\n",
           c->max_pkt_per_msg, c->max_xfer_size,
           c->pkt_alignment_factor);
    return 0;
}

static int rndis_query(uint32 oid, void *out, uint32 *out_len)
{
    uint8 req[128], rsp[512];
    struct rndis_message_header *mh = (struct rndis_message_header *)req;
    struct rndis_query_request *q =
        (struct rndis_query_request *)(req + sizeof(*mh));
    uint32 req_id = rndis_next_req_id();
    uint32 rsp_len = sizeof(rsp);

    memset(req, 0, sizeof(req));
    mh->msg_type = RNDIS_MSG_QUERY;
    mh->msg_len = sizeof(*mh) + sizeof(*q);
    q->req_id = req_id;
    q->oid = oid;
    q->info_buf_offset = sizeof(*q);

    int ret = rndis_send_control(req, mh->msg_len, req_id,
                                 RNDIS_MSG_QUERY_C, rsp, &rsp_len);
    if (ret != 0)
        return ret;
    if (rsp_len < sizeof(*mh) + sizeof(struct rndis_query_complete))
        return -EIO;
    struct rndis_query_complete *c =
        (struct rndis_query_complete *)(rsp + sizeof(*mh));
    if (c->status != RNDIS_STATUS_SUCCESS)
        return -EIO;
    if (c->info_buf_offset < sizeof(*c) ||
        c->info_buflen > *out_len ||
        sizeof(*mh) + c->info_buf_offset + c->info_buflen > rsp_len)
        return -EIO;
    memcpy(out, (uint8 *)c + c->info_buf_offset, c->info_buflen);
    *out_len = c->info_buflen;
    return 0;
}

static int rndis_set_u32(uint32 oid, uint32 value)
{
    uint8 req[128], rsp[128];
    struct rndis_message_header *mh = (struct rndis_message_header *)req;
    struct rndis_set_request *s =
        (struct rndis_set_request *)(req + sizeof(*mh));
    uint32 req_id = rndis_next_req_id();
    uint32 rsp_len = sizeof(rsp);

    memset(req, 0, sizeof(req));
    mh->msg_type = RNDIS_MSG_SET;
    mh->msg_len = sizeof(*mh) + sizeof(*s) + sizeof(value);
    s->req_id = req_id;
    s->oid = oid;
    s->info_buflen = sizeof(value);
    s->info_buf_offset = sizeof(*s);
    memcpy((uint8 *)s + s->info_buf_offset, &value, sizeof(value));

    int ret = rndis_send_control(req, mh->msg_len, req_id,
                                 RNDIS_MSG_SET_C, rsp, &rsp_len);
    if (ret != 0)
        return ret;
    if (rsp_len < sizeof(*mh) + sizeof(struct rndis_set_complete))
        return -EIO;
    struct rndis_set_complete *c =
        (struct rndis_set_complete *)(rsp + sizeof(*mh));
    return c->status == RNDIS_STATUS_SUCCESS ? 0 : -EIO;
}

static void hvnet_send_receive_complete(uint64 trans_id, uint32 status)
{
    struct nvsp_message msg;
    uint32 len = sizeof(struct nvsp_message_header) +
        sizeof(struct nvsp_1_message_send_rndis_packet_complete);

    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE;
    msg.msg.v1.send_rndis_pkt_complete.status = status;
    (void)hv_send_packet_type_on(hvnet.out_ring, hvnet.child_relid,
                                 hvnet.signal_conn_id,
                                 hvnet.monitor_allocated,
                                 hvnet.monitorid, hvnet.dedicated,
                                 VM_PKT_COMP, &msg, len, trans_id, 0);
}

static void hvnet_handle_rndis_completion(const uint8 *data, uint32 len)
{
    const struct rndis_message_header *mh =
        (const struct rndis_message_header *)data;
    uint32 req_id = 0;

    if (mh->msg_type == RNDIS_MSG_INIT_C &&
        len >= sizeof(*mh) + sizeof(struct rndis_initialize_complete)) {
        const struct rndis_initialize_complete *c =
            (const struct rndis_initialize_complete *)(data + sizeof(*mh));
        req_id = c->req_id;
    } else if (mh->msg_type == RNDIS_MSG_QUERY_C &&
               len >= sizeof(*mh) + sizeof(struct rndis_query_complete)) {
        const struct rndis_query_complete *c =
            (const struct rndis_query_complete *)(data + sizeof(*mh));
        req_id = c->req_id;
    } else if (mh->msg_type == RNDIS_MSG_SET_C &&
               len >= sizeof(*mh) + sizeof(struct rndis_set_complete)) {
        const struct rndis_set_complete *c =
            (const struct rndis_set_complete *)(data + sizeof(*mh));
        req_id = c->req_id;
    }
    if (req_id == 0)
        return;
    uint32 n = len > sizeof(hvnet.rndis_response) ?
        sizeof(hvnet.rndis_response) : len;
    memcpy(hvnet.rndis_response, data, n);
    hvnet.rndis_response_len = n;
    hvnet.rndis_response_type = mh->msg_type;
    hvnet.rndis_response_req_id = req_id;
    __atomic_store_n(&hvnet.rndis_response_pending, 1,
                     __ATOMIC_RELEASE);
}

static void hvnet_handle_rndis_packet(const uint8 *data, uint32 len)
{
    const struct rndis_message_header *mh;

    if (len < sizeof(*mh))
        return;
    mh = (const struct rndis_message_header *)data;
    if (mh->msg_len == 0 || mh->msg_len > len)
        return;

    if (mh->msg_type == RNDIS_MSG_PACKET) {
        if (mh->msg_len < sizeof(*mh) + sizeof(struct rndis_packet))
            return;
        const struct rndis_packet *pkt =
            (const struct rndis_packet *)(data + sizeof(*mh));
        uint32 data_off = sizeof(*mh) + pkt->data_offset;
        if (pkt->data_len == 0 ||
            data_off > mh->msg_len ||
            pkt->data_len > mh->msg_len - data_off)
            return;
        if (pkt->data_len > MBUF_SIZE) {
            hvnet.rx_drops++;
            return;
        }
        struct mbuf *m = mbufalloc(0);
        if (m == NULL) {
            hvnet.rx_drops++;
            return;
        }
        memcpy(mbufput(m, pkt->data_len), data + data_off, pkt->data_len);
        hvnet.rx_packets++;
        g_net_rx_packets++;
        g_net_rx_bytes += m->len;
        net_rx(m);
    } else if (mh->msg_type == RNDIS_MSG_INIT_C ||
               mh->msg_type == RNDIS_MSG_QUERY_C ||
               mh->msg_type == RNDIS_MSG_SET_C) {
        hvnet_handle_rndis_completion(data, mh->msg_len);
    } else if (mh->msg_type == RNDIS_MSG_INDICATE &&
               mh->msg_len >= sizeof(*mh) + sizeof(struct rndis_indicate_status)) {
        const struct rndis_indicate_status *s =
            (const struct rndis_indicate_status *)(data + sizeof(*mh));
        if (s->status == RNDIS_STATUS_MEDIA_CONNECT)
            netdev_set_link(&hvnet.ndev, 1);
        else if (s->status == RNDIS_STATUS_MEDIA_DISCONNECT)
            netdev_set_link(&hvnet.ndev, 0);
    }
}

static void hvnet_handle_xfer_packet(const uint8 *pkt, uint32 len,
                                     uint64 trans_id)
{
    const struct vmtransfer_page_packet_header *x =
        (const struct vmtransfer_page_packet_header *)pkt;
    uint32 need = sizeof(*x) + x->range_cnt * sizeof(x->ranges[0]);
    uint32 status = NVSP_STAT_SUCCESS;

    if (len < sizeof(*x) || need > len ||
        x->xfer_pageset_id != NETVSC_RECEIVE_BUFFER_ID) {
        hvnet_send_receive_complete(trans_id, NVSP_STAT_FAIL);
        return;
    }
    for (uint32 i = 0; i < x->range_cnt; i++) {
        uint32 off = x->ranges[i].byte_offset;
        uint32 n = x->ranges[i].byte_count;
        if (off > hvnet.recv_buf_size || n > hvnet.recv_buf_size - off) {
            status = NVSP_STAT_FAIL;
            continue;
        }
        hvnet_handle_rndis_packet(hvnet.recv_buf + off, n);
    }
    hvnet_send_receive_complete(trans_id, status);
}

static void hvnet_process_channel_packets(void)
{
    uint8 pkt[4096];
    uint32 len;
    uint16 type;

    for (int i = 0; i < 128; i++) {
        if (hv_recv_raw_on(hvnet.in_ring, pkt, sizeof(pkt), &len,
                           &type) != 0 || len == 0)
            return;
        struct vmpacket_descriptor *desc = (struct vmpacket_descriptor *)pkt;
        uint32 off = desc->offset8 << 3;
        uint32 plen = desc->len8 << 3;
        if (hv_debug_enabled() && hvnet.debug_rx_count < 32) {
            uint32 mt = 0;
            if (off <= plen && plen <= len &&
                plen - off >= sizeof(struct nvsp_message_header))
                mt = ((struct nvsp_message *)(pkt + off))->hdr.msg_type;
            printf("hyperv-netvsc: rx pkt type=0x%x len=%u off=%u plen=%u msg=%u trans=%lx\n",
                   type, len, off, plen, mt, desc->trans_id);
            hvnet.debug_rx_count++;
        }
        if (off > plen || plen > len)
            continue;
        if ((type == VM_PKT_DATA_INBAND || type == VM_PKT_COMP) &&
            plen - off >= sizeof(struct nvsp_message_header)) {
            const struct nvsp_message *msg =
                (const struct nvsp_message *)(pkt + off);
            hvnet_complete_nvsp(msg, desc->trans_id);
        } else if (type == VM_PKT_DATA_USING_XFER_PAGES) {
            hvnet_handle_xfer_packet(pkt, plen, desc->trans_id);
        }
    }
}

static int hvnet_negotiate_nvsp(uint32 version)
{
    struct nvsp_message msg, rsp;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = NVSP_MSG_TYPE_INIT;
    msg.msg.init.min_protocol_ver = version;
    msg.msg.init.max_protocol_ver = version;
    __atomic_store_n(&hvnet.response_pending, 0, __ATOMIC_RELEASE);
    if (hvnet_send_nvsp(&msg, VM_PKT_COMPLETION_REQUESTED) != 0)
        return -EIO;
    if (hvnet_wait_nvsp(NVSP_MSG_TYPE_INIT_COMPLETE, &rsp) != 0)
        return -ETIMEDOUT;
    if (rsp.msg.init_complete.status != NVSP_STAT_SUCCESS)
        return -EINVAL;
    hvnet.nvsp_version = version;
    printf("hyperv-netvsc: NVSP 0x%x accepted mdl=%u\n",
           version, rsp.msg.init_complete.max_mdl_chain_len);
    return 0;
}

static int hvnet_send_ndis_config(void)
{
    struct nvsp_message msg;

    if (hvnet.nvsp_version == NVSP_PROTOCOL_VERSION_1)
        return 0;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = NVSP_MSG2_TYPE_SEND_NDIS_CONFIG;
    msg.msg.v2.send_ndis_config.mtu = 1514;
    msg.msg.v2.send_ndis_config.capability.data =
        (1ULL << 2) |  /* SR-IOV capable */
        (1ULL << 3) |  /* 802.1Q */
        (1ULL << 5) |  /* teaming/link updates */
        (1ULL << 7);   /* RSC */
    return hvnet_send_nvsp(&msg, 0);
}

static int hvnet_send_ndis_version(void)
{
    struct nvsp_message msg;
    uint32 ndis_version = hvnet.nvsp_version <= NVSP_PROTOCOL_VERSION_4 ?
        0x00060001U : 0x0006001eU;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = NVSP_MSG1_TYPE_SEND_NDIS_VER;
    msg.msg.v1.send_ndis_ver.ndis_major_ver =
        (ndis_version >> 16) & 0xffff;
    msg.msg.v1.send_ndis_ver.ndis_minor_ver = ndis_version & 0xffff;
    return hvnet_send_nvsp(&msg, 0);
}

static void hvnet_wait_out_empty(void)
{
    for (int i = 0; i < HV_NET_WAIT_LOOPS; i++) {
        if (hvnet.out_ring->read_index == hvnet.out_ring->write_index)
            return;
        hv_process_messages();
        hv_process_events();
        if (hvnet.open_ok)
            hvnet_process_channel_packets();
        sleep_ms(1);
    }
    printf("hyperv-netvsc: outbound did not drain read=%u write=%u\n",
           hvnet.out_ring->read_index, hvnet.out_ring->write_index);
}

static int hvnet_send_recv_buf(void)
{
    struct nvsp_message msg, rsp;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = NVSP_MSG1_TYPE_SEND_RECV_BUF;
    msg.msg.v1.send_recv_buf.gpadl_handle = HV_NET_RECV_GPADL_HANDLE;
    msg.msg.v1.send_recv_buf.id = NETVSC_RECEIVE_BUFFER_ID;
    __atomic_store_n(&hvnet.response_pending, 0, __ATOMIC_RELEASE);
    if (hvnet_send_nvsp(&msg, VM_PKT_COMPLETION_REQUESTED) != 0)
        return -EIO;
    if (hvnet_wait_nvsp(NVSP_MSG1_TYPE_SEND_RECV_BUF_COMPLETE,
                        &rsp) != 0)
        return -ETIMEDOUT;
    if (rsp.msg.v1.send_recv_buf_complete.status != NVSP_STAT_SUCCESS)
        return -EIO;
    hvnet.recv_section_size = NETVSC_RECV_SECTION_SIZE;
    if (rsp.msg.v1.send_recv_buf_complete.num_sections > 0 &&
        rsp.msg.v1.send_recv_buf_complete.sections[0].sub_alloc_size != 0)
        hvnet.recv_section_size =
            rsp.msg.v1.send_recv_buf_complete.sections[0].sub_alloc_size;
    printf("hyperv-netvsc: receive buffer accepted sections=%u section=%u\n",
           rsp.msg.v1.send_recv_buf_complete.num_sections,
           hvnet.recv_section_size);
    return 0;
}

static int hvnet_send_send_buf(void)
{
    struct nvsp_message msg, rsp;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.msg_type = NVSP_MSG1_TYPE_SEND_SEND_BUF;
    msg.msg.v1.send_send_buf.gpadl_handle = HV_NET_SEND_GPADL_HANDLE;
    msg.msg.v1.send_send_buf.id = NETVSC_SEND_BUFFER_ID;
    __atomic_store_n(&hvnet.response_pending, 0, __ATOMIC_RELEASE);
    if (hvnet_send_nvsp(&msg, VM_PKT_COMPLETION_REQUESTED) != 0)
        return -EIO;
    if (hvnet_wait_nvsp(NVSP_MSG1_TYPE_SEND_SEND_BUF_COMPLETE,
                        &rsp) != 0)
        return -ETIMEDOUT;
    if (rsp.msg.v1.send_send_buf_complete.status != NVSP_STAT_SUCCESS)
        return -EIO;
    hvnet.send_section_size =
        rsp.msg.v1.send_send_buf_complete.section_size;
    if (hvnet.send_section_size == 0 ||
        hvnet.send_section_size > hvnet.send_buf_size)
        hvnet.send_section_size = NETVSC_SEND_SECTION_SIZE;
    hvnet.send_section_count = hvnet.send_buf_size / hvnet.send_section_size;
    if (hvnet.send_section_count > HV_NET_MAX_SEND_SECTIONS) {
        printf("hyperv-netvsc: send sections capped %u -> %u\n",
               hvnet.send_section_count, HV_NET_MAX_SEND_SECTIONS);
        hvnet.send_section_count = HV_NET_MAX_SEND_SECTIONS;
    }
    memset(hvnet.send_section_busy, 0, sizeof(hvnet.send_section_busy));
    __atomic_store_n(&hvnet.tx_inflight, 0, __ATOMIC_RELEASE);
    printf("hyperv-netvsc: send buffer accepted section=%u size=%u count=%u\n",
           hvnet.send_section_size, hvnet.send_buf_size,
           hvnet.send_section_count);
    return 0;
}

static int hvnet_transmit(struct netdev *dev, struct mbuf *m)
{
    (void)dev;
    if (!hvnet.initialized || m == NULL)
        return -EIO;
    uint32 data_off = sizeof(struct rndis_message_header) +
        sizeof(struct rndis_packet);
    uint32 msg_len = data_off + m->len;

    if (msg_len > hvnet.send_section_size || hvnet.send_section_count == 0)
        return -EINVAL;

    mutex_lock(&hvnet.tx_lock);
    hv_process_messages();
    hv_process_events();
    if (hvnet.open_ok)
        hvnet_process_channel_packets();

    int section = hvnet_alloc_send_section();
    if (section < 0) {
        mutex_unlock(&hvnet.tx_lock);
        return section;
    }
    uint8 *buf = hvnet.send_buf + (uint32)section * hvnet.send_section_size;

    memset(buf, 0, msg_len);
    struct rndis_message_header *mh = (struct rndis_message_header *)buf;
    struct rndis_packet *pkt =
        (struct rndis_packet *)(buf + sizeof(*mh));

    mh->msg_type = RNDIS_MSG_PACKET;
    mh->msg_len = msg_len;
    pkt->data_offset = sizeof(*pkt);
    pkt->data_len = m->len;
    memcpy(buf + data_off, m->head, m->len);

    int ret = hvnet_send_rndis_section((uint32)section, mh->msg_len);
    if (ret == 0) {
        hvnet.tx_packets++;
        g_net_tx_packets++;
        g_net_tx_bytes += m->len;
        mbuffree(m);
    }
    mutex_unlock(&hvnet.tx_lock);
    return ret;
}

static void hvvideo_complete(const struct synthvid_msg *rsp)
{
    if (hv_debug_enabled() && hvvideo.debug_rx_count < 32) {
        printf("hyperv-video: rx pipe=%u pipe_size=%u type=%u size=%u\n",
               rsp->pipe_type, rsp->pipe_size, rsp->hdr.type,
               rsp->hdr.size);
        hvvideo.debug_rx_count++;
    }
    if (rsp->hdr.type == SYNTHVID_FEATURE_CHANGE) {
        hvvideo.dirt_needed = rsp->feature.dirt_needed != 0;
        printf("hyperv-video: feature change dirt=%d ptr_pos=%d ptr_shape=%d situ=%d\n",
               rsp->feature.dirt_needed, rsp->feature.ptr_pos_needed,
               rsp->feature.ptr_shape_needed, rsp->feature.situ_needed);
        return;
    }
    if (rsp->hdr.type == SYNTHVID_VERSION_RESPONSE ||
        rsp->hdr.type == SYNTHVID_VRAM_LOCATION_ACK ||
        rsp->hdr.type == SYNTHVID_RESOLUTION_RESPONSE) {
        hvvideo.response = *rsp;
        __atomic_store_n(&hvvideo.response_pending, 1, __ATOMIC_RELEASE);
    }
}

static void hvvideo_process_channel_packets(void)
{
    uint8 pkt[1024];
    uint32 len;
    uint16 type;

    for (int i = 0; i < 64; i++) {
        if (hv_recv_raw_on(hvvideo.in_ring, pkt, sizeof(pkt), &len,
                           &type) != 0 || len == 0)
            return;
        if (type != VM_PKT_DATA_INBAND && type != VM_PKT_COMP)
            continue;
        struct vmpacket_descriptor *desc = (struct vmpacket_descriptor *)pkt;
        uint32 off = desc->offset8 << 3;
        uint32 plen = desc->len8 << 3;
        if (off > plen || plen > len ||
            plen - off < sizeof(uint32) * 3)
            continue;
        const struct synthvid_msg *msg =
            (const struct synthvid_msg *)(pkt + off);
        if (msg->pipe_type != PIPE_MSG_DATA)
            continue;
        hvvideo_complete(msg);
    }
}

static int hvvideo_wait_response(uint32 type, struct synthvid_msg *out)
{
    for (int i = 0; i < HV_WAIT_LOOPS; i++) {
        hv_process_messages();
        hv_process_events();
        if (hvvideo.open_ok)
            hvvideo_process_channel_packets();
        if (__atomic_load_n(&hvvideo.response_pending, __ATOMIC_ACQUIRE) &&
            hvvideo.response.hdr.type == type) {
            if (out)
                *out = hvvideo.response;
            __atomic_store_n(&hvvideo.response_pending, 0,
                             __ATOMIC_RELEASE);
            return 0;
        }
        sleep_ms(10);
    }
    if (__atomic_load_n(&hvvideo.response_pending, __ATOMIC_ACQUIRE))
        printf("hyperv-video: timed out waiting type=%u last type=%u size=%u\n",
               type, hvvideo.response.hdr.type, hvvideo.response.hdr.size);
    else
        printf("hyperv-video: timed out waiting type=%u with no response\n",
               type);
    return -ETIMEDOUT;
}

static int hvvideo_send_msg(struct synthvid_msg *msg)
{
    msg->pipe_type = PIPE_MSG_DATA;
    msg->pipe_size = msg->hdr.size;
    if (hv_debug_enabled() && hvvideo.debug_tx_count < 32) {
        printf("hyperv-video: tx type=%u size=%u len=%u\n",
               msg->hdr.type, msg->hdr.size, msg->hdr.size + 8);
        hvvideo.debug_tx_count++;
    }
    int ret = hv_send_packet_on(hvvideo.out_ring, hvvideo.child_relid,
                                hvvideo.signal_conn_id,
                                hvvideo.monitor_allocated,
                                hvvideo.monitorid, hvvideo.dedicated, msg,
                                msg->hdr.size + 8, (uint64)msg, 0);
    if (ret != 0)
        printf("hyperv-video: tx type=%u failed ret=%d\n",
               msg->hdr.type, ret);
    return ret;
}

static void hvvideo_queue_dirty_rect(uint32 x1, uint32 y1, uint32 x2, uint32 y2)
{
    if (!platform.has_framebuffer)
        return;
    if (x1 >= platform.framebuffer_width || y1 >= platform.framebuffer_height)
        return;
    if (x2 > platform.framebuffer_width)
        x2 = platform.framebuffer_width;
    if (y2 > platform.framebuffer_height)
        y2 = platform.framebuffer_height;
    if (x1 >= x2 || y1 >= y2)
        return;

    spin_lock(&hvvideo_dirty_lock);
    if (!hvvideo.dirty_pending) {
        hvvideo.dirty_x1 = x1;
        hvvideo.dirty_y1 = y1;
        hvvideo.dirty_x2 = x2;
        hvvideo.dirty_y2 = y2;
        hvvideo.dirty_pending = 1;
    } else {
        if (x1 < hvvideo.dirty_x1)
            hvvideo.dirty_x1 = x1;
        if (y1 < hvvideo.dirty_y1)
            hvvideo.dirty_y1 = y1;
        if (x2 > hvvideo.dirty_x2)
            hvvideo.dirty_x2 = x2;
        if (y2 > hvvideo.dirty_y2)
            hvvideo.dirty_y2 = y2;
    }
    spin_unlock(&hvvideo_dirty_lock);
}

static void hvvideo_flush_dirty(int force)
{
    struct synthvid_msg msg;
    uint32 x1, y1, x2, y2;
    uint64 now;

    if (!hvvideo.initialized)
        return;

    now = sched_timer_now_ms();
    if (!force && hvvideo.dirty_last_ms != 0 &&
        now - hvvideo.dirty_last_ms < HV_VIDEO_DIRTY_INTERVAL_MS)
        return;

    spin_lock(&hvvideo_dirty_lock);
    if (!hvvideo.dirty_pending) {
        spin_unlock(&hvvideo_dirty_lock);
        return;
    }
    x1 = hvvideo.dirty_x1;
    y1 = hvvideo.dirty_y1;
    x2 = hvvideo.dirty_x2;
    y2 = hvvideo.dirty_y2;
    hvvideo.dirty_pending = 0;
    spin_unlock(&hvvideo_dirty_lock);

    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = SYNTHVID_DIRT;
    msg.hdr.size = sizeof(struct synthvid_msg_hdr) +
                   sizeof(struct synthvid_dirt);
    msg.dirt.output = 0;
    msg.dirt.count = 1;
    msg.dirt.rect[0].x1 = (int32)x1;
    msg.dirt.rect[0].y1 = (int32)y1;
    msg.dirt.rect[0].x2 = (int32)x2;
    msg.dirt.rect[0].y2 = (int32)y2;
    if (hvvideo_send_msg(&msg) != 0) {
        hvvideo_queue_dirty_rect(x1, y1, x2, y2);
        return;
    }
    hvvideo.dirty_last_ms = now;
}

static void hvvideo_refresh_if_idle(void)
{
    uint64 now;

    if (!hvvideo.initialized || !platform.has_framebuffer)
        return;

    now = sched_timer_now_ms();
    if (hvvideo.dirty_last_ms != 0 &&
        now - hvvideo.dirty_last_ms < HV_VIDEO_REFRESH_INTERVAL_MS)
        return;

    spin_lock(&hvvideo_dirty_lock);
    if (hvvideo.dirty_pending) {
        spin_unlock(&hvvideo_dirty_lock);
        return;
    }
    spin_unlock(&hvvideo_dirty_lock);

    hvvideo_queue_dirty_rect(0, 0, platform.framebuffer_width,
                             platform.framebuffer_height);
    hvvideo_flush_dirty(1);
}

static int hvvideo_negotiate(uint32 version)
{
    struct synthvid_msg msg, rsp;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = SYNTHVID_VERSION_REQUEST;
    msg.hdr.size = sizeof(struct synthvid_msg_hdr) +
                   sizeof(struct synthvid_version_req);
    msg.version_req.version = version;
    __atomic_store_n(&hvvideo.response_pending, 0, __ATOMIC_RELEASE);
    if (hvvideo_send_msg(&msg) != 0)
        return -EIO;
    if (hvvideo_wait_response(SYNTHVID_VERSION_RESPONSE, &rsp) != 0)
        return -ETIMEDOUT;
    if (!rsp.version_resp.accepted)
        return -EINVAL;
    printf("hyperv-video: synthvid %lu.%lu accepted outputs=%u\n",
           (uint64)(version & 0xffff), (uint64)(version >> 16),
           rsp.version_resp.max_outputs);
    return 0;
}

static int hvvideo_set_vram(uint64 gpa)
{
    struct synthvid_msg msg, rsp;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = SYNTHVID_VRAM_LOCATION;
    msg.hdr.size = sizeof(struct synthvid_msg_hdr) +
                   sizeof(struct synthvid_vram_location);
    msg.vram.user_ctx = gpa;
    msg.vram.is_vram_gpa_specified = 1;
    msg.vram.vram_gpa = gpa;
    __atomic_store_n(&hvvideo.response_pending, 0, __ATOMIC_RELEASE);
    if (hvvideo_send_msg(&msg) != 0)
        return -EIO;
    if (hvvideo_wait_response(SYNTHVID_VRAM_LOCATION_ACK, &rsp) != 0)
        return -ETIMEDOUT;
    return rsp.vram_ack.user_ctx == gpa ? 0 : -EIO;
}

static void hvvideo_update_situation(void)
{
    struct synthvid_msg msg;

    if (!platform.has_framebuffer)
        return;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = SYNTHVID_SITUATION_UPDATE;
    msg.hdr.size = sizeof(struct synthvid_msg_hdr) +
                   sizeof(struct synthvid_situation);
    msg.situation.output_count = 1;
    msg.situation.active = 1;
    msg.situation.vram_offset = 0;
    msg.situation.depth_bits = platform.framebuffer_bpp;
    msg.situation.width_pixels = platform.framebuffer_width;
    msg.situation.height_pixels = platform.framebuffer_height;
    msg.situation.pitch_bytes = platform.framebuffer_pitch;
    (void)hvvideo_send_msg(&msg);
}

static void hvvideo_request_resolutions(void)
{
    struct synthvid_msg msg, rsp;

    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = SYNTHVID_RESOLUTION_REQUEST;
    msg.hdr.size = sizeof(struct synthvid_msg_hdr) +
                   sizeof(struct synthvid_resolution_req);
    msg.resolution_req.maximum_resolution_count =
        SYNTHVID_MAX_RESOLUTION_COUNT;
    __atomic_store_n(&hvvideo.response_pending, 0, __ATOMIC_RELEASE);
    if (hvvideo_send_msg(&msg) != 0 ||
        hvvideo_wait_response(SYNTHVID_RESOLUTION_RESPONSE, &rsp) != 0)
        return;
    printf("hyperv-video: supported resolutions count=%u default=%u\n",
           rsp.resolution_resp.resolution_count,
           rsp.resolution_resp.default_resolution_index);
    uint8 n = rsp.resolution_resp.resolution_count;
    if (n > 8)
        n = 8;
    for (uint8 i = 0; i < n; i++)
        printf("hyperv-video: mode[%u]=%ux%u\n", i,
               rsp.resolution_resp.supported[i].width,
               rsp.resolution_resp.supported[i].height);
}

static int hvvideo_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvvideo.child_relid, HV_VIDEO_GPADL_HANDLE,
                                    hvvideo.ring_pa, HV_RING_PAGES * PGSIZE,
                                    &hvvideo.gpadl_ok,
                                    &hvvideo.gpadl_status);
}

static int hvpci_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvpci.child_relid, HV_PCI_GPADL_HANDLE,
                                    hvpci.ring_pa, HV_RING_PAGES * PGSIZE,
                                    &hvpci.gpadl_ok,
                                    &hvpci.gpadl_status);
}

static int hvdxg_global_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvdxg.global_relid,
                                    HV_DXG_GLOBAL_GPADL_HANDLE,
                                    hvdxg.global_ring_pa,
                                    HV_RING_PAGES * PGSIZE,
                                    &hvdxg.global_gpadl_ok,
                                    &hvdxg.global_gpadl_status);
}

static int hvdxg_vgpu_establish_gpadl(void)
{
    return hv_establish_gpadl_large(hvdxg.vgpu_relid,
                                    HV_DXG_VGPU_GPADL_HANDLE,
                                    hvdxg.vgpu_ring_pa,
                                    HV_RING_PAGES * PGSIZE,
                                    &hvdxg.vgpu_gpadl_ok,
                                    &hvdxg.vgpu_gpadl_status);
}

