static int hvdxg_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    return hvdxg_ioctl_common(cdev, cmd, arg, NULL);
}

static ssize_t hvdxg_fops_read(struct vfs_file *file, char *buf,
                               size_t count, bool user)
{
    struct hvdxg_open_state *owner =
        file ? (struct hvdxg_open_state *)file->private_data : NULL;

    if (owner == NULL)
        return -EBADF;
    return hvdxg_read_status(&hvdxg_cdev, user, buf, count,
                             &owner->read_offset, &owner->read_emitted,
                             &owner->read_status, &owner->read_status_len);
}

static int hvdxg_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct hvdxg_open_state *owner =
        file ? (struct hvdxg_open_state *)file->private_data : NULL;

    if (owner == NULL)
        return -EBADF;
    return hvdxg_ioctl_common(&hvdxg_cdev, cmd, arg, owner);
}

static int hvdxg_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    struct hvdxg_open_state *owner =
        file ? (struct hvdxg_open_state *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (owner != NULL) {
        hvdxg_cleanup_open_state(owner);
        hvdxg_free_open_state(owner);
    }
    return hvdxg_release(&hvdxg_cdev);
}

static struct vfs_file_ops hvdxg_file_ops = {
    .read = hvdxg_fops_read,
    .ioctl = hvdxg_fops_ioctl,
    .release = hvdxg_fops_release,
};

static int hvdxg_open_file(cdev_t *cdev, struct vfs_file *file)
{
    struct hvdxg_open_state *owner;
    struct hvdxg_process_state *process;
    int ret = hvdxg_open(cdev);

    if (ret != 0)
        return ret;
    process = hvdxg_process_get_current();
    if (process == NULL) {
        (void)hvdxg_release(cdev);
        return -ENOMEM;
    }
    owner = kvmalloc(sizeof(*owner));
    if (owner == NULL) {
        (void)hvdxg_process_put(process);
        (void)hvdxg_release(cdev);
        return -ENOMEM;
    }
    memset(owner, 0, sizeof(*owner));
    owner->process_state = process;
    owner->dxg_process = process->host_process;
    owner->dxg_process_guest = process->guest_process;
    owner->dxg_process_pid = process->pid;
    owner->dxg_process_created = process->host_process_created;
    mutex_lock(&hvdxg.process_lock);
    if (hvdxg_should_defer_open_create(process))
        ret = 0;
    else
        ret = hvdxg_bind_open_process(owner);
    mutex_unlock(&hvdxg.process_lock);
    if (ret != 0) {
        /*
         * WSL creates the host dxgprocess before open returns. Keep that
         * eager attempt for parity, but do not make diagnostics depend on it:
         * /dev/dxg reads are how users can see why CREATEPROCESS failed.
         * Real D3DKMT ioctls call hvdxg_bind_open_process() again before
         * forwarding host commands, so a deferred fd either retries there or
         * fails the ioctl with the recorded create-process diagnostics.
         */
        hvdxg.open_createprocess_ignored_failures++;
        process->host_process.v = 0;
        process->host_process_created = 0;
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
        hvdxg.d3dkmt_ready = 0;
    }
    file->ops = &hvdxg_file_ops;
    file->private_data = owner;
    return 0;
}

static cdev_t hvdxg_cdev = {
    .dev = {
        .major = 31,
        .minor = 0,
        .devname = "dxg",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 0,
    .ops = {
        .read = hvdxg_read,
        .open = hvdxg_open,
        .release = hvdxg_release,
        .ioctl = hvdxg_ioctl,
        .open_file = hvdxg_open_file,
    },
};

static void hvdxg_register_status_device(void)
{
    if (hvdxg.cdev_registered ||
        (!hvdxg.global_present && !hvdxg.vgpu_present))
        return;
    int ret = cdev_register(&hvdxg_cdev);
    if (ret == 0) {
        hvdxg.cdev_registered = 1;
        printf("hyperv-dxg: registered /dev/dxg status node\n");
    } else {
        printf("hyperv-dxg: /dev/dxg registration failed: %d\n", ret);
    }
}

int hyperv_dxg_transport_ready(void)
{
    return hvdxg.global_open_ok && hvdxg.vgpu_open_ok;
}

int hyperv_dxg_d3dkmt_ready(void)
{
    if (hvdxg.d3dkmt_ready)
        return 1;
    return hvdxg_d3dkmt_ensure() == 0;
}

int hyperv_dxg_get_status(struct hyperv_dxg_status *status)
{
    struct hvdxg_winluid user_luid;
    struct d3dkmt_adaptertype adapter_type;
    uint32 adapter_type_raw;
    uint32 adapter_type_wsl;

    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    user_luid = hvdxg_user_adapter_luid(NULL);
    adapter_type_raw = hvdxg.queryadapter_adaptertype_raw_value;
    adapter_type_wsl = hvdxg.queryadapter_adaptertype_wsl_value;
    if (adapter_type_wsl == 0 &&
        hvdxg.queryadapter_adaptertype_last_wsl_value != 0) {
        adapter_type_raw = hvdxg.queryadapter_adaptertype_last_raw_value;
        adapter_type_wsl = hvdxg.queryadapter_adaptertype_last_wsl_value;
    }
    adapter_type.value = adapter_type_wsl;
    status->global_present = hvdxg.global_present;
    status->vgpu_present = hvdxg.vgpu_present;
    status->global_gpadl_ok = hvdxg.global_gpadl_ok;
    status->vgpu_gpadl_ok = hvdxg.vgpu_gpadl_ok;
    status->global_open_ok = hvdxg.global_open_ok;
    status->vgpu_open_ok = hvdxg.vgpu_open_ok;
    status->global_relid = hvdxg.global_relid;
    status->vgpu_relid = hvdxg.vgpu_relid;
    status->global_gpadl_status = hvdxg.global_gpadl_status;
    status->vgpu_gpadl_status = hvdxg.vgpu_gpadl_status;
    status->global_open_status = hvdxg.global_open_status;
    status->vgpu_open_status = hvdxg.vgpu_open_status;
    status->global_rx_packets = hvdxg.global_rx_packets;
    status->vgpu_rx_packets = hvdxg.vgpu_rx_packets;
    status->adapter_type_rewrites =
        hvdxg.queryadapter_adaptertype_rewrite_count;
    status->adapter_type_raw_value = adapter_type_raw;
    status->adapter_type_wsl_value = adapter_type_wsl;
    status->adapter_render_supported = adapter_type.render_supported;
    status->adapter_display_supported = adapter_type.display_supported;
    status->adapter_paravirtualized = adapter_type.paravirtualized;
    status->adapter_compute_only = adapter_type.compute_only;
    status->adapter_source_count = hvdxg.enumadapters_last_num_sources;
    status->adapter_sources_known =
        hvdxg.enumadapters_last_cmd != 0 &&
        (hvdxg.enumadapters_last_out_count != 0 ||
         hvdxg.enumadapters_last_ret == 0);
    status->enum_adapter_count = hvdxg.enumadapters_last_out_count;
    status->enum_adapter_handle = hvdxg.enumadapters_last_handle;
    status->enum_adapter_luid_low = hvdxg.enumadapters_last_luid_low;
    status->enum_adapter_luid_high = hvdxg.enumadapters_last_luid_high;
    status->user_adapter_luid_low = user_luid.a;
    status->user_adapter_luid_high = user_luid.b;
    return (hvdxg.global_present || hvdxg.vgpu_present) ? 0 : -ENODEV;
}

