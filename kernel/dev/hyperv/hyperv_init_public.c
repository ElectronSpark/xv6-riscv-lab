static int hvdxg_open_channel(uint32 relid, uint32 gpadl,
                              volatile int *open_ok)
{
    struct vmbus_open_channel msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.msgtype = CHANNELMSG_OPENCHANNEL;
    msg.child_relid = relid;
    msg.openid = relid;
    msg.ringbuffer_gpadlhandle = gpadl;
    msg.target_vp = 0;
    msg.downstream_ringbuffer_pageoffset = HV_SEND_PAGES;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(open_ok);
}

static int hvvideo_open_channel(void)
{
    struct vmbus_open_channel msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.msgtype = CHANNELMSG_OPENCHANNEL;
    msg.child_relid = hvvideo.child_relid;
    msg.openid = hvvideo.child_relid;
    msg.ringbuffer_gpadlhandle = HV_VIDEO_GPADL_HANDLE;
    msg.target_vp = 0;
    msg.downstream_ringbuffer_pageoffset = HV_SEND_PAGES;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(&hvvideo.open_ok);
}

static int hvstor_wait_completion(uint64 trans_id, struct vstor_packet *out,
                                  uint64 timeout_ms)
{
    uint64 start = sched_timer_now_ms();
    if (timeout_ms == 0)
        timeout_ms = HV_STOR_INIT_TIMEOUT_MS;

    for (;;) {
        hv_process_messages();
        hv_process_events();
        if (hvstor.open_ok)
            hvstor_process_channel_packets();
        if (__atomic_load_n(&hvstor.completion_pending, __ATOMIC_ACQUIRE) &&
            hvstor.completion_trans_id == trans_id) {
            if (out != NULL)
                *out = hvstor.completion;
            __atomic_store_n(&hvstor.completion_pending, 0,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&hvstor.waiting_trans_id, 0, __ATOMIC_RELEASE);
            return 0;
        }
        if (sched_timer_now_ms() - start >= timeout_ms)
            break;
        sleep_ms(1);
    }
    __atomic_store_n(&hvstor.waiting_trans_id, 0, __ATOMIC_RELEASE);
    return -ETIMEDOUT;
}

static int hvstor_send_vstor(struct vstor_packet *req, struct vstor_packet *rsp)
{
    uint64 trans_id = hvstor_next_trans_id();
    __atomic_store_n(&hvstor.completion_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvstor.waiting_trans_id, trans_id, __ATOMIC_RELEASE);
    int ret = hv_send_packet_on(hvstor.out_ring, hvstor.child_relid,
                                hvstor.signal_conn_id,
                                hvstor.monitor_allocated, hvstor.monitorid,
                                hvstor.dedicated,
                                req, sizeof(*req), trans_id,
                                VM_PKT_COMPLETION_REQUESTED);
    if (ret != 0) {
        __atomic_store_n(&hvstor.waiting_trans_id, 0, __ATOMIC_RELEASE);
        return ret;
    }
    ret = hvstor_wait_completion(trans_id, rsp, HV_STOR_INIT_TIMEOUT_MS);
    if (ret != 0)
        return ret;
    return rsp->status == 0 ? 0 : -EIO;
}

static int hvstor_send_srb(uint8 opcode, uint64 sector, void *buf, uint32 len,
                           int data_in, struct vstor_packet *rsp)
{
    struct vstor_packet req;
    struct hv_multipage_buffer mpb;
    uint64 trans_id = hvstor_next_trans_id();
    int ret;

    memset(&req, 0, sizeof(req));
    req.operation = VSTOR_OPERATION_EXECUTE_SRB;
    req.flags = REQUEST_COMPLETION_FLAG;
    req.vm_srb.length = sizeof(struct vmscsi_request);
    req.vm_srb.port_number = (uint8)hvstor.port;
    req.vm_srb.path_id = (uint8)hvstor.path_id;
    req.vm_srb.target_id = (uint8)hvstor.target_id;
    req.vm_srb.lun = 0;
    req.vm_srb.cdb_len = 10;
    req.vm_srb.data_in = (uint8)data_in;
    req.vm_srb.data_transfer_length = len;
    req.vm_srb.time_out_value = 60;
    req.vm_srb.srb_flags = SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
    if (data_in == STORVSC_DATA_READ)
        req.vm_srb.srb_flags |= SRB_FLAGS_DATA_IN;
    else if (data_in == STORVSC_DATA_WRITE)
        req.vm_srb.srb_flags |= SRB_FLAGS_DATA_OUT;
    else
        req.vm_srb.srb_flags |= SRB_FLAGS_NO_DATA_TRANSFER;
    req.vm_srb.cdb[0] = opcode;

    if (opcode == SCSI_READ_10 || opcode == SCSI_WRITE_10) {
        uint32 blocks = len / hvstor.sector_size;
        be32_put(&req.vm_srb.cdb[2], (uint32)sector);
        be16_put(&req.vm_srb.cdb[7], (uint16)blocks);
    } else if (opcode == SCSI_INQUIRY) {
        req.vm_srb.cdb_len = 6;
        req.vm_srb.cdb[4] = (uint8)len;
    } else if (opcode == SCSI_READ_CAPACITY_10) {
        req.vm_srb.data_transfer_length = 8;
    } else if (opcode == SCSI_TEST_UNIT_READY ||
               opcode == SCSI_SYNCHRONIZE_CACHE) {
        req.vm_srb.data_transfer_length = 0;
        req.vm_srb.data_in = STORVSC_DATA_NONE;
    }

    __atomic_store_n(&hvstor.completion_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvstor.waiting_trans_id, trans_id, __ATOMIC_RELEASE);
    if (buf != NULL && len != 0) {
        uint64 pa = (uint64)buf;
        memset(&mpb, 0, sizeof(mpb));
        mpb.len = len;
        mpb.offset = pa & (PGSIZE - 1);
        uint32 page_count = (mpb.offset + len + PGSIZE - 1) >> PGSHIFT;
        if (page_count > HV_STOR_MAX_PAGES) {
            __atomic_store_n(&hvstor.waiting_trans_id, 0, __ATOMIC_RELEASE);
            return -EINVAL;
        }
        uint64 base = pa & ~(uint64)(PGSIZE - 1);
        for (uint32 i = 0; i < page_count; i++)
            mpb.pfn_array[i] = (base + (uint64)i * PGSIZE) >> PGSHIFT;
        ret = hv_send_packet_mpb_on(hvstor.out_ring, hvstor.child_relid,
                                    hvstor.signal_conn_id,
                                    hvstor.monitor_allocated,
                                    hvstor.monitorid, hvstor.dedicated,
                                    &mpb, &req,
                                    sizeof(req), trans_id);
    } else {
        ret = hv_send_packet_on(hvstor.out_ring, hvstor.child_relid,
                                hvstor.signal_conn_id,
                                hvstor.monitor_allocated, hvstor.monitorid,
                                hvstor.dedicated,
                                &req, sizeof(req), trans_id,
                                VM_PKT_COMPLETION_REQUESTED);
    }
    if (ret != 0) {
        __atomic_store_n(&hvstor.waiting_trans_id, 0, __ATOMIC_RELEASE);
        return ret;
    }
    ret = hvstor_wait_completion(trans_id, rsp, HV_STOR_IO_TIMEOUT_MS);
    if (ret != 0)
        return ret;
    if (rsp->status != 0 ||
        SRB_STATUS(rsp->vm_srb.srb_status) != SRB_STATUS_SUCCESS ||
        rsp->vm_srb.scsi_status != 0)
        return -EIO;
    return 0;
}

static int hvstor_init_protocol(void)
{
    struct vstor_packet req, rsp;
    static const uint16 versions[] = {
        VMSTOR_PROTOCOL_WIN10, VMSTOR_PROTOCOL_WIN8_1,
        VMSTOR_PROTOCOL_WIN8, VMSTOR_PROTOCOL_WIN7,
    };
    int ret;

    memset(&req, 0, sizeof(req));
    req.operation = VSTOR_OPERATION_BEGIN_INITIALIZATION;
    ret = hvstor_send_vstor(&req, &rsp);
    if (ret != 0)
        return ret;

    ret = -EIO;
    for (uint32 i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        memset(&req, 0, sizeof(req));
        req.operation = VSTOR_OPERATION_QUERY_PROTOCOL;
        req.version.major_minor = versions[i];
        if (hvstor_send_vstor(&req, &rsp) == 0) {
            hvstor.protocol_ok = 1;
            ret = 0;
            break;
        }
    }
    if (ret != 0)
        return ret;

    memset(&req, 0, sizeof(req));
    req.operation = VSTOR_OPERATION_QUERY_PROPERTIES;
    ret = hvstor_send_vstor(&req, &rsp);
    if (ret != 0)
        return ret;
    hvstor.path_id = 0;
    hvstor.target_id = 0;
    hvstor.port = 0;
    hvstor.max_transfer_bytes =
        rsp.storage_channel_properties.max_transfer_bytes;
    if (hvstor.max_transfer_bytes == 0 ||
        hvstor.max_transfer_bytes > HV_STOR_MAX_PAGES * PGSIZE)
        hvstor.max_transfer_bytes = HV_STOR_MAX_PAGES * PGSIZE;

    memset(&req, 0, sizeof(req));
    req.operation = VSTOR_OPERATION_END_INITIALIZATION;
    ret = hvstor_send_vstor(&req, &rsp);
    return ret;
}

static int hvstor_probe_capacity(void)
{
    uint8 *probe_page = page_alloc(0, PAGE_TYPE_ANON);
    uint8 *inquiry = probe_page;
    uint8 *cap = probe_page != NULL ? probe_page + 64 : NULL;
    struct vstor_packet rsp;
    int ret;

    if (probe_page == NULL)
        return -ENOMEM;

    memset(probe_page, 0, PGSIZE);
    ret = hvstor_send_srb(SCSI_INQUIRY, 0, inquiry, 36,
                          STORVSC_DATA_READ, &rsp);
    if (ret != 0)
        goto out;

    ret = hvstor_send_srb(SCSI_TEST_UNIT_READY, 0, NULL, 0,
                          STORVSC_DATA_NONE, &rsp);
    if (ret != 0)
        goto out;

    ret = hvstor_send_srb(SCSI_READ_CAPACITY_10, 0, cap, 8,
                          STORVSC_DATA_READ, &rsp);
    if (ret != 0)
        goto out;

    uint32 last_lba = be32_get(&cap[0]);
    uint32 block_size = be32_get(&cap[4]);
    if (block_size == 0)
        block_size = BLK_SIZE;
    hvstor.sector_size = block_size;
    hvstor.sectors = (uint64)last_lba + 1;
    hvstor.blkdev.block_shift = 0;
    while ((BLK_SIZE << hvstor.blkdev.block_shift) < block_size &&
           hvstor.blkdev.block_shift < 7)
        hvstor.blkdev.block_shift++;
    printf("hyperv-storvsc: disk capacity %lu sectors, sector=%u max-xfer=%u\n",
           hvstor.sectors, hvstor.sector_size, hvstor.max_transfer_bytes);
    ret = 0;
out:
    page_free(probe_page, 0);
    return ret;
}

static int hvstor_open(blkdev_t *blkdev)
{
    (void)blkdev;
    return 0;
}

static int hvstor_release(blkdev_t *blkdev)
{
    (void)blkdev;
    return 0;
}

static int hvstor_transfer(uint64 sector, void *pa, uint32 len, int write)
{
    struct vstor_packet rsp;
    uint32 max_len = hvstor.max_transfer_bytes;
    if (max_len == 0 || max_len > HV_STOR_MAX_PAGES * PGSIZE)
        max_len = HV_STOR_MAX_PAGES * PGSIZE;
    max_len -= max_len % hvstor.sector_size;
    if (max_len == 0)
        return -EINVAL;

    while (len != 0) {
        uint32 chunk = len > max_len ? max_len : len;
        uint32 page_room = HV_STOR_MAX_PAGES * PGSIZE -
            ((uint64)pa & (PGSIZE - 1));
        if (chunk > page_room)
            chunk = page_room - (page_room % hvstor.sector_size);
        if (chunk == 0 || (chunk % hvstor.sector_size) != 0)
            return -EINVAL;
        int ret = hvstor_send_srb(write ? SCSI_WRITE_10 : SCSI_READ_10,
                                  sector, pa, chunk,
                                  write ? STORVSC_DATA_WRITE :
                                          STORVSC_DATA_READ,
                                  &rsp);
        if (ret != 0) {
            if (ret == -ETIMEDOUT) {
                uint64 n = __atomic_add_fetch(&hvstor.io_timeouts, 1,
                                              __ATOMIC_RELAXED);
                if (n <= 8)
                    printf("hyperv-storvsc: %s timeout sector=%lu len=%u\n",
                           write ? "write" : "read", sector, chunk);
            }
            return ret;
        }
        sector += chunk / hvstor.sector_size;
        pa = (uint8 *)pa + chunk;
        len -= chunk;
    }
    return 0;
}

static int hvstor_submit_bio(blkdev_t *blkdev, struct bio *bio)
{
    struct bio_vec bvec;
    struct bio_iter iter;
    int ret = 0;
    (void)blkdev;

    bio_start_io_acct(bio);
    bio->inflight_segs = 1;
    bio->completed_segs = 0;

    mutex_lock(&hvstor.io_lock);
    bio_for_each_segment(&bvec, bio, &iter) {
        void *pa = (void *)__page_to_pa(bvec.bv_page);
        ret = hvstor_transfer(iter.blkno, (uint8 *)pa + bvec.offset,
                              bvec.len, bio_dir_write(bio));
        if (ret != 0)
            break;
    }
    mutex_unlock(&hvstor.io_lock);

    bio->error = ret;
    bio_complete(bio);
    return 0;
}

static int hvstor_flush(blkdev_t *blkdev)
{
    struct vstor_packet rsp;
    (void)blkdev;
    if (hvstor.flush_disabled)
        return 0;
    if (!hv_cmdline_enabled("hyperv_sync_cache")) {
        hvstor.flush_disabled = 1;
        if (!hvstor.flush_warned) {
            hvstor.flush_warned = 1;
            printf("hyperv-storvsc: volatile cache flush disabled "
                   "(set hyperv_sync_cache=1 to enable)\n");
        }
        return 0;
    }
    mutex_lock(&hvstor.io_lock);
    int ret = hvstor_send_srb(SCSI_SYNCHRONIZE_CACHE, 0, NULL, 0,
                              STORVSC_DATA_NONE, &rsp);
    mutex_unlock(&hvstor.io_lock);
    if (ret == -ETIMEDOUT) {
        hvstor.flush_disabled = 1;
        if (!hvstor.flush_warned) {
            hvstor.flush_warned = 1;
            printf("hyperv-storvsc: SYNCHRONIZE CACHE timed out; disabling "
                   "volatile cache flushes\n");
        }
        return 0;
    }
    return ret;
}

static blkdev_ops_t hvstor_ops = {
    .open = hvstor_open,
    .release = hvstor_release,
    .submit_bio = hvstor_submit_bio,
    .flush = hvstor_flush,
};

static void hv_poll_thread(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;
    for (;;) {
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
        if (hvvideo.open_ok) {
            hvvideo_process_channel_packets();
            hvvideo_flush_dirty(0);
            hvvideo_refresh_if_idle();
        }
        hvdxg_pump_channels();
        sleep_ms(4);
    }
}

static int hv_connect_protocol(void)
{
    struct mousevsc_msg req;

    memset(&req, 0, sizeof(req));
    req.type = PIPE_MESSAGE_DATA;
    req.size = sizeof(struct synthhid_protocol_request);
    req.request.header.type = SYNTH_HID_PROTOCOL_REQUEST;
    req.request.header.size = sizeof(uint32);
    req.request.version = SYNTHHID_INPUT_VERSION;

    if (hv_send_packet(&req, 8 + sizeof(struct synthhid_protocol_request),
                       (uint64)&req, VM_PKT_COMPLETION_REQUESTED) != 0)
        return -EIO;
    if (hv_wait_flag(&hv.protocol_ok) != 0)
        return -ETIMEDOUT;
    return hv_wait_flag(&hv.device_info_ok);
}

static int hvkbd_connect_protocol(void)
{
    struct synthkbd_protocol_request req;

    memset(&req, 0, sizeof(req));
    req.type = SYNTH_KBD_PROTOCOL_REQUEST;
    req.version = SYNTH_KBD_VERSION;

    if (hv_send_packet_on(hvkbd.out_ring, hvkbd.child_relid,
                          hvkbd.signal_conn_id, hvkbd.monitor_allocated,
                          hvkbd.monitorid, hvkbd.dedicated, &req,
                          sizeof(req), (uint64)&req,
                          VM_PKT_COMPLETION_REQUESTED) != 0)
        return -EIO;
    return hv_wait_flag(&hvkbd.protocol_ok);
}

static int hv_request_offers(void)
{
    struct vmbus_msg_hdr msg;

    memset(&msg, 0, sizeof(msg));
    msg.msgtype = CHANNELMSG_REQUESTOFFERS;
    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    if (hv_wait_flag(&hv.all_offers) != 0)
        return -ETIMEDOUT;
    return 0;
}

static int hv_negotiate(void)
{
    struct vmbus_initiate_contact msg;

    memset(&msg, 0, sizeof(msg));
    hv.msg_conn_id = VMBUS_MSG_CONN_ID4;
    msg.header.msgtype = CHANNELMSG_INITIATE_CONTACT;
    msg.version = VMBUS_VERSION_WIN10_V5;
    msg.target_vcpu = 0;
    msg.msg_sint = HV_MESSAGE_SINT;
    msg.msg_vtl = 0;
    msg.monitor_page1 = hv.monitor1_pa;
    msg.monitor_page2 = hv.monitor2_pa;

    if (hv_post_msg(hv.msg_conn_id, &msg, sizeof(msg)) != 0)
        return -EIO;
    return hv_wait_flag(&hv.connected);
}

static int hv_setup_synic(void)
{
    uint64 guest_id = ((uint64)0x8100 << 48) | ((uint64)0x00060000 << 16);

    if (hv_alloc_page(&hv.hypercall_pa, &hv.hypercall_page) != 0 ||
        hv_alloc_page(&hv.post_pa, &hv.post_page) != 0 ||
        hv_alloc_page(&hv.msg_pa, (void **)&hv.msg_page) != 0 ||
        hv_alloc_page(&hv.event_pa, &hv.event_page) != 0 ||
        hv_alloc_page(&hv.int_pa, &hv.int_page) != 0 ||
        hv_alloc_page(&hv.monitor1_pa, &hv.monitor1) != 0 ||
        hv_alloc_page(&hv.monitor2_pa, &hv.monitor2) != 0)
        return -ENOMEM;
    hv.recv_int_page = hv.int_page;
    hv.send_int_page = (uint8 *)hv.int_page + (PGSIZE / 2);

    wrmsr(HV_MSR_GUEST_OS_ID, guest_id);
    wrmsr(HV_MSR_HYPERCALL, (hv.hypercall_pa & ~0xfffULL) | 1);
    wrmsr(HV_MSR_SIMP, (hv.msg_pa & ~0xfffULL) | HV_SYNIC_PAGE_ENABLE);
    wrmsr(HV_MSR_SIEFP, (hv.event_pa & ~0xfffULL) | HV_SYNIC_PAGE_ENABLE);
    wrmsr(HV_MSR_SINT(HV_MESSAGE_SINT), HV_SYNIC_VECTOR);
    wrmsr(HV_MSR_SCONTROL, rdmsr(HV_MSR_SCONTROL) | HV_SYNIC_ENABLE);
    return 0;
}

void hyperv_input_init(void)
{
    if (!hv_is_hyperv())
        return;

    hv.present = 1;
    mutex_init(&hvdxg.process_lock, "hvdxg_proc");
    spin_init(&hvpci.config_lock, "hvpci_cfg");
    printf("hyperv-input: probing VMBus synthetic input\n");

    if (hv_setup_synic() != 0) {
        printf("hyperv-input: SynIC setup failed\n");
        return;
    }
    hv.ring_pa = (uint64)page_alloc(HV_RING_ORDER, PAGE_TYPE_ANON);
    if (hv.ring_pa == 0) {
        printf("hyperv-input: ring allocation failed\n");
        return;
    }
    hv.ring = (uint8 *)hv.ring_pa;
    hv_ring_init();

    if (hv_negotiate() != 0) {
        printf("hyperv-input: VMBus negotiation failed\n");
        return;
    }
    if (hv_request_offers() != 0) {
        printf("hyperv-input: mouse offer not found\n");
        return;
    }
        printf("hyperv-vmbus: offers mouse=%d keyboard=%d storage=%d netvsc=%d video=%d dxg_global=%d dxg_vgpu=%d unknown=%u\n",
           hv.child_relid != 0, hvkbd.present, hvstor.present,
            hvnet.present, hvvideo.present, hvdxg.global_present,
            hvdxg.vgpu_count, hv_unknown_offer_count);
        hvdxg_register_status_device();
    if (hvkbd.present) {
        hvkbd.ring_pa = (uint64)page_alloc(HV_RING_ORDER, PAGE_TYPE_ANON);
        if (hvkbd.ring_pa == 0) {
            printf("hyperv-input: keyboard ring allocation failed\n");
            hvkbd.present = 0;
        } else {
            hvkbd.ring = (uint8 *)hvkbd.ring_pa;
            hvkbd_ring_init();
        }
    }
    if (hv.child_relid != 0) {
        if (hv_establish_gpadl() != 0) {
            printf("hyperv-input: GPADL failed status=%u\n", hv.gpadl_status);
            return;
        }
        if (hv_open_channel() != 0) {
            printf("hyperv-input: open failed status=%u\n", hv.open_status);
            return;
        }
        if (hv_connect_protocol() != 0) {
            printf("hyperv-input: synthhid protocol failed\n");
            return;
        }
    } else {
        printf("hyperv-input: mouse offer not found\n");
    }
    if (hvkbd.present) {
        if (hvkbd_establish_gpadl() != 0) {
            printf("hyperv-input: keyboard GPADL failed status=%u\n",
                   hvkbd.gpadl_status);
            hvkbd.present = 0;
        } else if (hvkbd_open_channel() != 0) {
            printf("hyperv-input: keyboard open failed status=%u\n",
                   hvkbd.open_status);
            hvkbd.present = 0;
        } else if (hvkbd_connect_protocol() != 0) {
            printf("hyperv-input: keyboard protocol failed\n");
            hvkbd.present = 0;
        } else {
            printf("hyperv-input: synthetic keyboard online\n");
        }
    } else {
        printf("hyperv-input: keyboard offer not found\n");
    }

    if (hvvideo.present && platform.has_framebuffer) {
        hvvideo.ring_pa = (uint64)page_alloc(HV_RING_ORDER, PAGE_TYPE_ANON);
        if (hvvideo.ring_pa == 0) {
            printf("hyperv-video: ring allocation failed\n");
        } else {
            hvvideo.ring = (uint8 *)hvvideo.ring_pa;
            hvvideo_ring_init();
            if (hvvideo_establish_gpadl() != 0) {
                printf("hyperv-video: GPADL failed status=%u\n",
                       hvvideo.gpadl_status);
            } else if (hvvideo_open_channel() != 0) {
                printf("hyperv-video: open failed status=%u\n",
                       hvvideo.open_status);
            } else if (hvvideo_negotiate(SYNTHVID_VERSION_WIN10) != 0 &&
                       hvvideo_negotiate(SYNTHVID_VERSION_WIN8) != 0) {
                printf("hyperv-video: protocol negotiation failed\n");
            } else if (hvvideo_set_vram(platform.framebuffer_base) != 0) {
                printf("hyperv-video: VRAM location failed\n");
            } else {
                hvvideo.initialized = 1;
                hvvideo_request_resolutions();
                hvvideo_update_situation();
                hvvideo_queue_dirty_rect(0, 0, platform.framebuffer_width,
                                         platform.framebuffer_height);
                hvvideo_flush_dirty(1);
                printf("hyperv-video: online using firmware framebuffer 0x%lx %ux%u pitch=%u\n",
                       platform.framebuffer_base, platform.framebuffer_width,
                       platform.framebuffer_height, platform.framebuffer_pitch);
            }
        }
    } else if (hvvideo.present) {
        printf("hyperv-video: offer present but no firmware framebuffer\n");
    } else {
        printf("hyperv-video: synthetic video offer not found\n");
    }

    if (hvpci.present) {
        hvpci.ring_pa = (uint64)page_alloc(HV_RING_ORDER, PAGE_TYPE_ANON);
        if (hvpci.ring_pa == 0) {
            hvpci.protocol_last_ret = -ENOMEM;
            printf("hyperv-pci: ring allocation failed\n");
        } else {
            hvpci.ring = (uint8 *)hvpci.ring_pa;
            hvpci_ring_init();
            if (hvpci_establish_gpadl() != 0) {
                hvpci.protocol_last_ret = -EIO;
                printf("hyperv-pci: GPADL failed status=%u\n",
                       hvpci.gpadl_status);
            } else if (hvdxg_open_channel(hvpci.child_relid,
                                          HV_PCI_GPADL_HANDLE,
                                          &hvpci.open_ok) != 0) {
                hvpci.protocol_last_ret = -EIO;
                printf("hyperv-pci: open failed status=%u\n",
                       hvpci.open_status);
            } else if (hvpci_negotiate_protocol() != 0) {
                printf("hyperv-pci: protocol negotiation failed version=0x%x status=%d ret=%d\n",
                       hvpci.protocol_last_version,
                       hvpci.protocol_last_status,
                       hvpci.protocol_last_ret);
            } else {
                (void)hvpci_register_backend();
                (void)hvpci_config_window_init();
                (void)hvpci_bar_window_init();
                (void)hvpci_enter_d0();
                (void)hvpci_query_bus_relations();
                printf("hyperv-pci: channel open relid=%u version=0x%x\n",
                       hvpci.child_relid,
                       hvpci.protocol_selected_version);
            }
        }
    } else {
        printf("hyperv-pci: offer not found\n");
    }

    if (hvdxg.global_present) {
        hvdxg.global_ring_pa = (uint64)page_alloc(HV_RING_ORDER,
                                                  PAGE_TYPE_ANON);
        if (hvdxg.global_ring_pa == 0) {
            printf("hyperv-dxg: global ring allocation failed\n");
        } else {
            hvdxg.global_ring = (uint8 *)hvdxg.global_ring_pa;
            hvdxg_ring_init(hvdxg.global_ring, &hvdxg.global_out_ring,
                            &hvdxg.global_in_ring);
            if (hvdxg_global_establish_gpadl() != 0) {
                printf("hyperv-dxg: global GPADL failed status=%u\n",
                       hvdxg.global_gpadl_status);
            } else if (hvdxg_open_channel(hvdxg.global_relid,
                                          HV_DXG_GLOBAL_GPADL_HANDLE,
                                          &hvdxg.global_open_ok) != 0) {
                printf("hyperv-dxg: global open failed status=%u\n",
                       hvdxg.global_open_status);
            } else {
                printf("hyperv-dxg: global transport open relid=%u\n",
                       hvdxg.global_relid);
                if (hvdxg_try_pci_guestcaps_scan() == -ENODEV)
                    hvdxg_note_missing_pci_guestcaps_once();
                (void)hvdxg_set_iospace_region();
            }
        }
    }
    if (hvdxg.vgpu_present) {
        hvdxg.vgpu_ring_pa = (uint64)page_alloc(HV_RING_ORDER,
                                                PAGE_TYPE_ANON);
        if (hvdxg.vgpu_ring_pa == 0) {
            printf("hyperv-dxg: vgpu ring allocation failed\n");
        } else {
            hvdxg.vgpu_ring = (uint8 *)hvdxg.vgpu_ring_pa;
            hvdxg_ring_init(hvdxg.vgpu_ring, &hvdxg.vgpu_out_ring,
                            &hvdxg.vgpu_in_ring);
            if (hvdxg_vgpu_establish_gpadl() != 0) {
                printf("hyperv-dxg: vgpu GPADL failed status=%u\n",
                       hvdxg.vgpu_gpadl_status);
            } else if (hvdxg_open_channel(hvdxg.vgpu_relid,
                                          HV_DXG_VGPU_GPADL_HANDLE,
                                          &hvdxg.vgpu_open_ok) != 0) {
                printf("hyperv-dxg: vgpu open failed status=%u\n",
                       hvdxg.vgpu_open_status);
            } else {
                printf("hyperv-dxg: vgpu transport open relid=%u\n",
                       hvdxg.vgpu_relid);
                int dxg_ret = hvdxg_probe_transport();
                printf("hyperv-dxg: vgpu probe ret=%d attempts=%u successes=%u status=%d handle=0x%x info_len=%u\n",
                       dxg_ret, hvdxg.probe_attempts,
                       hvdxg.probe_successes, hvdxg.probe_open_status,
                       hvdxg.probe_open_handle, hvdxg.probe_info_len);
            }
        }
    } else {
        printf("hyperv-dxg: GPU-PV offer not found\n");
    }

    struct thread *poller = kthread_create("hyperv_input",
                                           hv_poll_thread, 0, 0, 0);
    if (IS_ERR_OR_NULL(poller))
        printf("hyperv-input: failed to start poll thread\n");
    else if (hv.child_relid != 0)
        printf("hyperv-input: synthetic mouse online\n");
}

void hyperv_storvsc_init(void)
{
    if (!hv_is_hyperv())
        return;
    if (!hv.present || !hv.connected || !hv.all_offers) {
        printf("hyperv-storvsc: VMBus is not initialized\n");
        return;
    }
    if (!hvstor.present || hvstor.child_relid == 0) {
        printf("hyperv-storvsc: storage offer not found\n");
        return;
    }
    if (hvstor.initialized)
        return;

    mutex_init(&hvstor.io_lock, "hvstor");
    hvstor.sector_size = BLK_SIZE;
    hvstor.ring_pa = (uint64)page_alloc(HV_RING_ORDER, PAGE_TYPE_ANON);
    if (hvstor.ring_pa == 0) {
        printf("hyperv-storvsc: ring allocation failed\n");
        return;
    }
    hvstor.ring = (uint8 *)hvstor.ring_pa;
    hvstor_ring_init();

    if (hvstor_establish_gpadl() != 0) {
        printf("hyperv-storvsc: GPADL failed status=%u\n",
               hvstor.gpadl_status);
        return;
    }
    if (hvstor_open_channel() != 0) {
        printf("hyperv-storvsc: open failed status=%u\n",
               hvstor.open_status);
        return;
    }
    if (hvstor_init_protocol() != 0) {
        printf("hyperv-storvsc: protocol initialization failed\n");
        return;
    }
    if (hvstor_probe_capacity() != 0) {
        printf("hyperv-storvsc: capacity probe failed\n");
        return;
    }

    hvstor.blkdev.ops = hvstor_ops;
    int ret = blkdev_register(&hvstor.blkdev);
    if (ret != 0) {
        printf("hyperv-storvsc: blkdev_register failed: %d\n", ret);
        return;
    }
    hvstor.initialized = 1;
    gendisk_probe(&hvstor.blkdev);
    printf("hyperv-storvsc: disk0 online\n");
}

void hyperv_netvsc_init(void)
{
    static const uint32 versions[] = {
        NVSP_PROTOCOL_VERSION_61, NVSP_PROTOCOL_VERSION_6,
        NVSP_PROTOCOL_VERSION_5, NVSP_PROTOCOL_VERSION_4,
        NVSP_PROTOCOL_VERSION_2, NVSP_PROTOCOL_VERSION_1,
    };
    uint8 mac[6];
    uint32 mac_len = sizeof(mac);
    uint32 link = RNDIS_MEDIA_STATE_CONNECTED;
    uint32 link_len = sizeof(link);
    uint32 speed = 100000; /* RNDIS reports 100bps units. */
    uint32 speed_len = sizeof(speed);
    int ret = -EIO;

    if (!hv_is_hyperv())
        return;
    if (!hv.present || !hv.connected || !hv.all_offers) {
        printf("hyperv-netvsc: VMBus is not initialized\n");
        return;
    }
    if (!hvnet.present || hvnet.child_relid == 0) {
        printf("hyperv-netvsc: offer not found\n");
        return;
    }
    if (hvnet.initialized)
        return;

    mutex_init(&hvnet.tx_lock, "hvnet_tx");
    hvnet.ring_pa = (uint64)page_alloc(HV_RING_ORDER, PAGE_TYPE_ANON);
    hvnet.recv_buf_pa = (uint64)page_alloc(HV_NET_RECV_ORDER,
                                           PAGE_TYPE_ANON);
    hvnet.send_buf_pa = (uint64)page_alloc(HV_NET_SEND_ORDER,
                                           PAGE_TYPE_ANON);
    if (hvnet.ring_pa == 0 || hvnet.recv_buf_pa == 0 ||
        hvnet.send_buf_pa == 0) {
        printf("hyperv-netvsc: allocation failed ring=%lx recv=%lx send=%lx\n",
               hvnet.ring_pa, hvnet.recv_buf_pa, hvnet.send_buf_pa);
        return;
    }
    hvnet.ring = (uint8 *)hvnet.ring_pa;
    hvnet.recv_buf = (uint8 *)hvnet.recv_buf_pa;
    hvnet.send_buf = (uint8 *)hvnet.send_buf_pa;
    hvnet.recv_buf_size = HV_NET_RECV_SIZE;
    hvnet.send_buf_size = HV_NET_SEND_SIZE;
    memset(hvnet.recv_buf, 0, hvnet.recv_buf_size);
    memset(hvnet.send_buf, 0, hvnet.send_buf_size);
    hvnet_ring_init();

    if (hvnet_establish_gpadl() != 0) {
        printf("hyperv-netvsc: ring GPADL failed status=%u\n",
               hvnet.gpadl_status);
        return;
    }
    if (hvnet_open_channel() != 0) {
        printf("hyperv-netvsc: open failed status=%u\n",
               hvnet.open_status);
        return;
    }
    for (uint32 i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        ret = hvnet_negotiate_nvsp(versions[i]);
        if (ret == 0)
            break;
    }
    if (ret != 0) {
        printf("hyperv-netvsc: NVSP negotiation failed\n");
        return;
    }
    if (hvnet_send_ndis_config() != 0 ||
        hvnet_send_ndis_version() != 0) {
        printf("hyperv-netvsc: NDIS config failed\n");
        return;
    }
    if (hvnet_establish_recv_gpadl() != 0) {
        printf("hyperv-netvsc: recv GPADL failed status=%u\n",
               hvnet.recv_gpadl_status);
        return;
    }
    if (hvnet_send_recv_buf() != 0) {
        printf("hyperv-netvsc: receive buffer registration failed\n");
        return;
    }
    if (hvnet_establish_send_gpadl() != 0) {
        printf("hyperv-netvsc: send GPADL failed status=%u\n",
               hvnet.send_gpadl_status);
        return;
    }
    if (hvnet_send_send_buf() != 0) {
        printf("hyperv-netvsc: send buffer registration failed\n");
        return;
    }
    hvnet_wait_out_empty();
    if (rndis_init_device() != 0) {
        printf("hyperv-netvsc: RNDIS init failed\n");
        return;
    }
    if (rndis_query(RNDIS_OID_802_3_CURRENT_ADDRESS, mac, &mac_len) != 0 ||
        mac_len != sizeof(mac)) {
        mac_len = sizeof(mac);
        if (rndis_query(RNDIS_OID_802_3_PERMANENT_ADDRESS,
                        mac, &mac_len) != 0 || mac_len != sizeof(mac)) {
            printf("hyperv-netvsc: MAC query failed\n");
            return;
        }
    }
    (void)rndis_query(RNDIS_OID_GEN_MEDIA_CONNECT_STATUS, &link, &link_len);
    (void)rndis_query(RNDIS_OID_GEN_LINK_SPEED, &speed, &speed_len);
    uint32 filter = RNDIS_PACKET_TYPE_DIRECTED |
                    RNDIS_PACKET_TYPE_MULTICAST |
                    RNDIS_PACKET_TYPE_BROADCAST;
    if (rndis_set_u32(RNDIS_OID_GEN_CURRENT_PACKET_FILTER, filter) != 0) {
        printf("hyperv-netvsc: packet filter setup failed\n");
        return;
    }

    memset(&hvnet.ndev, 0, sizeof(hvnet.ndev));
    memset(&hvnet.ops, 0, sizeof(hvnet.ops));
    strncpy(hvnet.ndev.name, "netvsc0", NETDEV_NAME_MAX);
    memmove(hvnet.ndev.mac, mac, sizeof(mac));
    hvnet.ndev.mtu = 1500;
    hvnet.ndev.link_up = (link == RNDIS_MEDIA_STATE_CONNECTED);
    hvnet.ndev.speed = (int)(speed / 10000);
    if (hvnet.ndev.speed == 0)
        hvnet.ndev.speed = 10000;
    hvnet.ndev.full_duplex = 1;
    hvnet.ops.transmit = hvnet_transmit;
    hvnet.ndev.ops = &hvnet.ops;
    hvnet.ndev.priv = &hvnet;
    if (netdev_register(&hvnet.ndev) != 0) {
        printf("hyperv-netvsc: netdev registration failed\n");
        return;
    }
    hvnet.initialized = 1;
    printf("hyperv-netvsc: online MAC %x:%x:%x:%x:%x:%x link=%d speed=%dMbps rxbuf=%u\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           hvnet.ndev.link_up, hvnet.ndev.speed, hvnet.recv_buf_size);
}

void hyperv_video_dirty(uint32 x, uint32 y, uint32 w, uint32 h)
{
    if (!hvvideo.initialized || w == 0 || h == 0)
        return;
    hvvideo_queue_dirty_rect(x, y, x + w, y + h);
    hvvideo_flush_dirty(0);
}

int hyperv_video_get_status(struct hyperv_video_status *status)
{
    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    status->present = hvvideo.present;
    status->gpadl_ok = hvvideo.gpadl_ok;
    status->open_ok = hvvideo.open_ok;
    status->initialized = hvvideo.initialized;
    status->dirt_needed = hvvideo.dirt_needed;
    status->child_relid = hvvideo.child_relid;
    status->gpadl_status = hvvideo.gpadl_status;
    status->open_status = hvvideo.open_status;
    if (platform.has_framebuffer) {
        status->vram_gpa = platform.framebuffer_base;
        status->width = platform.framebuffer_width;
        status->height = platform.framebuffer_height;
        status->pitch = platform.framebuffer_pitch;
        status->bpp = platform.framebuffer_bpp;
    }
    return 0;
}

