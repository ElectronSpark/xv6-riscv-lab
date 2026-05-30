/*
 * Hyper-V DXG: shared-object (fd-backed) operations and resource snapshots.
 *
 * Part of the dev/hyperv unity translation unit (included by module.c).
 * Split out of the former hyperv_dxg_objects_shared.c for readability;
 * include order in module.c preserves the original definition order.
 */

static int hvdxg_shared_object_stat(struct vfs_file *file, struct stat *st)
{
    struct hvdxg_shared_object *shared;

    if (file == NULL || st == NULL)
        return -EINVAL;
    shared = (struct hvdxg_shared_object *)file->private_data;
    if (shared == NULL)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_ino = shared->kind == HV_DXG_SHARED_OBJECT_SYNC ?
        0x64786701ULL : 0x64786702ULL;
    st->st_blksize = 4096;
    return 0;
}

static ssize_t hvdxg_shared_object_readlink(struct vfs_file *file, char *buf,
                                            size_t buflen)
{
    struct hvdxg_shared_object *shared;
    const char *name;

    if (file == NULL || buf == NULL)
        return -EINVAL;
    shared = (struct hvdxg_shared_object *)file->private_data;
    if (shared == NULL)
        return -EINVAL;

    name = shared->kind == HV_DXG_SHARED_OBJECT_SYNC ?
        "anon_inode:dxgsyncobj" : "anon_inode:dxgresource";
    return snprintf(buf, buflen, "%s", name);
}

static struct vfs_file_ops hvdxg_shared_sync_file_ops = {
    .stat = hvdxg_shared_object_stat,
    .readlink = hvdxg_shared_object_readlink,
    .release = hvdxg_shared_object_release,
    .early_release_on_close = 1,
};

static struct vfs_file_ops hvdxg_shared_resource_file_ops = {
    .stat = hvdxg_shared_object_stat,
    .readlink = hvdxg_shared_object_readlink,
    .release = hvdxg_shared_object_release,
    .early_release_on_close = 1,
};

static struct vfs_file_ops hvdxg_file_ops;

static uint32 hvdxg_shared_object_fops_kind(struct vfs_file *file)
{
    if (file == NULL)
        return HV_DXG_SHARED_FOPS_NONE;
    if (file->ops == &hvdxg_shared_sync_file_ops)
        return HV_DXG_SHARED_FOPS_SYNC;
    if (file->ops == &hvdxg_shared_resource_file_ops)
        return HV_DXG_SHARED_FOPS_RESOURCE;
    return HV_DXG_SHARED_FOPS_NONE;
}

enum {
    HV_DXG_BIND_PIN_REASON_OK = 0,
    HV_DXG_BIND_PIN_REASON_BAD_ARGS = 1,
    HV_DXG_BIND_PIN_REASON_NO_FDTABLE = 2,
    HV_DXG_BIND_PIN_REASON_NO_SHARED_FD = 3,
    HV_DXG_BIND_PIN_REASON_BAD_SHARED_FD = 4,
    HV_DXG_BIND_PIN_REASON_BAD_PARENT = 5,
    HV_DXG_BIND_PIN_REASON_BAD_DXG_FD = 6,
    HV_DXG_BIND_PIN_REASON_NO_MATCHING_RESOURCE = 7,
};

static void hvdxg_note_display_bind_pin_diag(
    int ret, uint32 reason, int dxg_fd, int resource_fd, uint32 device,
    uint32 resource, uint32 allocation, uint32 allocation_count,
    struct hvdxg_shared_object *shared, uint32 fops_kind,
    struct hvdxg_tracked_resource *parent_resource,
    struct hvdxg_open_state *owner, struct hvdxg_tracked_resource *opened,
    uint32 selected)
{
    hvdxg.display_bind_pin_last_ret = ret;
    hvdxg.display_bind_pin_last_reason = reason;
    hvdxg.display_bind_pin_last_dxg_fd = dxg_fd;
    hvdxg.display_bind_pin_last_resource_fd = resource_fd;
    hvdxg.display_bind_pin_last_device = device;
    hvdxg.display_bind_pin_last_resource = resource;
    hvdxg.display_bind_pin_last_allocation = allocation;
    hvdxg.display_bind_pin_last_allocation_count = allocation_count;
    hvdxg.display_bind_pin_last_shared_kind = shared != NULL ?
        shared->kind : 0;
    hvdxg.display_bind_pin_last_fops_kind = fops_kind;
    hvdxg.display_bind_pin_last_shared_global = shared != NULL ?
        shared->global_share : 0;
    hvdxg.display_bind_pin_last_parent_present =
        parent_resource != NULL;
    hvdxg.display_bind_pin_last_parent_device =
        parent_resource != NULL ? parent_resource->device : 0;
    hvdxg.display_bind_pin_last_parent_resource =
        parent_resource != NULL ? parent_resource->resource : 0;
    hvdxg.display_bind_pin_last_parent_allocation =
        parent_resource != NULL &&
        parent_resource->allocation_count != 0 ?
        parent_resource->allocation_handles[0] : 0;
    hvdxg.display_bind_pin_last_parent_allocation_count =
        parent_resource != NULL ? parent_resource->allocation_count : 0;
    hvdxg.display_bind_pin_last_parent_global =
        parent_resource != NULL ? parent_resource->global_share : 0;
    hvdxg.display_bind_pin_last_parent_sealed =
        parent_resource != NULL ? parent_resource->sealed : 0;
    hvdxg.display_bind_pin_last_parent_records =
        parent_resource != NULL ?
        parent_resource->shared_records_valid : 0;
    hvdxg.display_bind_pin_last_parent_generation =
        parent_resource != NULL ? parent_resource->sealed_generation : 0;
    hvdxg.display_bind_pin_last_dxg_file_ok = owner != NULL;
    hvdxg.display_bind_pin_last_owner_process =
        owner != NULL ? hvdxg_open_host_process(owner) : 0;
    hvdxg.display_bind_pin_last_owner_generation =
        owner != NULL ? hvdxg_open_process_generation(owner) : 0;
    hvdxg.display_bind_pin_last_owner_refs =
        owner != NULL ? hvdxg_open_process_refs(owner) : 0;
    hvdxg.display_bind_pin_last_opened_present = opened != NULL;
    hvdxg.display_bind_pin_last_opened_device =
        opened != NULL ? opened->device : 0;
    hvdxg.display_bind_pin_last_opened_resource =
        opened != NULL ? opened->resource : 0;
    hvdxg.display_bind_pin_last_opened_allocation =
        opened != NULL && opened->allocation_count != 0 ?
        opened->allocation_handles[0] : 0;
    hvdxg.display_bind_pin_last_opened_allocation_count =
        opened != NULL ? opened->allocation_count : 0;
    hvdxg.display_bind_pin_last_opened_global =
        opened != NULL ? opened->global_share : 0;
    hvdxg.display_bind_pin_last_selected = selected;
}

static struct hvdxg_shared_object *hvdxg_shared_object_from_fd(
    int fd, uint32 kind, struct vfs_file **file_out)
{
    struct vfs_file *f;
    struct hvdxg_shared_object *shared;

    if (file_out != NULL)
        *file_out = NULL;
    if (fd < 0 || current == NULL || current->fdtable == NULL)
        return NULL;
    f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return NULL;
    if ((f->ops != &hvdxg_shared_sync_file_ops &&
         f->ops != &hvdxg_shared_resource_file_ops) ||
        f->private_data == NULL) {
        vfs_fput(f);
        return NULL;
    }
    shared = (struct hvdxg_shared_object *)f->private_data;
    if (kind != 0 && shared->kind != kind) {
        vfs_fput(f);
        return NULL;
    }
    if (file_out != NULL)
        *file_out = f;
    else
        vfs_fput(f);
    return shared;
}

int hyperv_dxg_shared_resource_snapshot_from_fd(
    int fd, struct hyperv_dxg_shared_resource_snapshot *snapshot)
{
    struct hvdxg_shared_object *shared;
    struct vfs_file *file = NULL;
    struct hvdxg_tracked_resource *resource;

    if (snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));

    shared = hvdxg_shared_object_from_fd(fd, 0, &file);
    if (shared == NULL)
        return -EINVAL;

    snapshot->kind = shared->kind;
    snapshot->fops_kind = hvdxg_shared_object_fops_kind(file);
    if (shared->kind != HV_DXG_SHARED_OBJECT_RESOURCE) {
        vfs_fput(file);
        return -EINVAL;
    }

    resource = hvdxg_shared_parent_resource(shared);
    if (resource == NULL) {
        vfs_fput(file);
        return -EINVAL;
    }
    snapshot->device = resource->device;
    snapshot->resource = resource->resource;
    snapshot->allocation_count = resource->allocation_count;
    snapshot->first_allocation = resource->allocation_count != 0 ?
        resource->allocation_handles[0] : 0;
    snapshot->sealed = resource->sealed;
    snapshot->shared_records_valid = resource->shared_records_valid;
    snapshot->generation = resource->sealed_generation;
    snapshot->host_shared_refs = resource->host_shared_refs;
    if (shared->resource_parent != NULL) {
        snapshot->shared_parent_id = shared->resource_parent->id;
        snapshot->shared_parent_refs = shared->resource_parent->refs;
        snapshot->shared_parent_children = shared->resource_parent->child_count;
        snapshot->shared_parent_fd_refs = shared->resource_parent->fd_refs;
        snapshot->shared_parent_host_nt_refs =
            shared->resource_parent->host_nt_refs;
        snapshot->shared_parent_child_refs =
            shared->resource_parent->child_refs;
        snapshot->shared_parent_global_share =
            shared->resource_parent->global_share;
        snapshot->shared_parent_host_nt_handle =
            shared->resource_parent->host_nt_handle;
    } else {
        snapshot->shared_parent_id = shared->parent_id;
        snapshot->shared_parent_refs = shared->parent_refs;
        snapshot->shared_parent_children = shared->opened_child_count;
        snapshot->shared_parent_fd_refs = shared->fd_refs;
        snapshot->shared_parent_host_nt_refs =
            shared->host_nt_handle != 0 ? 1 : 0;
        snapshot->shared_parent_child_refs = shared->opened_child_count;
        snapshot->shared_parent_global_share = shared->global_share;
        snapshot->shared_parent_host_nt_handle = shared->host_nt_handle;
    }
    vfs_fput(file);
    return snapshot->kind == HV_DXG_SHARED_OBJECT_RESOURCE &&
           snapshot->fops_kind == HV_DXG_SHARED_FOPS_RESOURCE &&
           snapshot->sealed != 0 &&
           snapshot->shared_records_valid != 0 &&
           snapshot->generation != 0 ? 0 : -EINVAL;
}

int hyperv_dxg_shared_resource_snapshot_from_opened_resource(
    int dxg_fd, int resource_fd, uint32 device, uint32 resource,
    uint32 allocation, uint32 allocation_count,
    struct hyperv_dxg_shared_resource_snapshot *snapshot)
{
    struct hvdxg_shared_object *shared;
    struct hvdxg_tracked_resource *opened;
    struct hvdxg_tracked_resource *parent_resource;
    struct vfs_file *shared_file = NULL;
    struct vfs_file *dxg_file = NULL;
    struct hvdxg_open_state *owner;
    uint32 fops_kind;
    int ret = -EINVAL;

    if (snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    if (dxg_fd < 0 || resource_fd < 0 || device == 0 || resource == 0 ||
        allocation == 0 || allocation_count == 0)
        return -EINVAL;
    if (current == NULL || current->fdtable == NULL)
        return -EINVAL;

    shared = hvdxg_shared_object_from_fd(resource_fd,
                                         HV_DXG_SHARED_OBJECT_RESOURCE,
                                         &shared_file);
    if (shared == NULL)
        return -EINVAL;
    fops_kind = hvdxg_shared_object_fops_kind(shared_file);
    if (fops_kind != HV_DXG_SHARED_FOPS_RESOURCE ||
        shared->global_share == 0)
        goto out;
    parent_resource = hvdxg_shared_parent_resource(shared);
    if (parent_resource == NULL || parent_resource->sealed == 0 ||
        parent_resource->shared_records_valid == 0 ||
        parent_resource->sealed_generation == 0)
        goto out;

    dxg_file = vfs_fdtable_get_file(current->fdtable, dxg_fd);
    if (dxg_file == NULL || dxg_file->ops != &hvdxg_file_ops ||
        dxg_file->private_data == NULL)
        goto out;
    owner = (struct hvdxg_open_state *)dxg_file->private_data;
    opened = hvdxg_owner_find_resource(owner, device, resource);
    if (opened == NULL || opened->allocation_count != allocation_count ||
        opened->allocation_count == 0 ||
        opened->allocation_count > HV_DXG_ALLOCATION_MAX ||
        opened->allocation_handles[0] != allocation ||
        opened->global_share != shared->global_share)
        goto out;

    snapshot->kind = shared->kind;
    snapshot->fops_kind = fops_kind;
    snapshot->device = opened->device;
    snapshot->resource = opened->resource;
    snapshot->allocation_count = opened->allocation_count;
    snapshot->first_allocation = opened->allocation_handles[0];
    snapshot->sealed = parent_resource->sealed;
    snapshot->shared_records_valid = parent_resource->shared_records_valid;
    snapshot->generation = parent_resource->sealed_generation;
    snapshot->host_shared_refs = parent_resource->host_shared_refs;
    if (shared->resource_parent != NULL) {
        snapshot->shared_parent_id = shared->resource_parent->id;
        snapshot->shared_parent_refs = shared->resource_parent->refs;
        snapshot->shared_parent_children = shared->resource_parent->child_count;
        snapshot->shared_parent_fd_refs = shared->resource_parent->fd_refs;
        snapshot->shared_parent_host_nt_refs =
            shared->resource_parent->host_nt_refs;
        snapshot->shared_parent_child_refs =
            shared->resource_parent->child_refs;
        snapshot->shared_parent_global_share =
            shared->resource_parent->global_share;
        snapshot->shared_parent_host_nt_handle =
            shared->resource_parent->host_nt_handle;
    } else {
        snapshot->shared_parent_id = shared->parent_id;
        snapshot->shared_parent_refs = shared->parent_refs;
        snapshot->shared_parent_children = shared->opened_child_count;
        snapshot->shared_parent_fd_refs = shared->fd_refs;
        snapshot->shared_parent_host_nt_refs =
            shared->host_nt_handle != 0 ? 1 : 0;
        snapshot->shared_parent_child_refs = shared->opened_child_count;
        snapshot->shared_parent_global_share = shared->global_share;
        snapshot->shared_parent_host_nt_handle = shared->host_nt_handle;
    }
    ret = 0;

out:
    if (dxg_file != NULL)
        vfs_fput(dxg_file);
    if (shared_file != NULL)
        vfs_fput(shared_file);
    return ret;
}

int hyperv_dxg_display_bind_pin_from_fds(
    int dxg_fd, int resource_fd, uint32 device, uint32 resource,
    uint32 allocation, uint32 allocation_count,
    struct hyperv_dxg_display_bind_pin_snapshot *snapshot)
{
    struct hvdxg_shared_object *shared;
    struct hvdxg_tracked_resource *opened = NULL;
    struct hvdxg_tracked_resource *pinned_resource = NULL;
    struct hvdxg_tracked_resource *parent_resource = NULL;
    struct vfs_file *shared_file = NULL;
    struct vfs_file *dxg_file = NULL;
    struct hvdxg_open_state *owner = NULL;
    struct hvdxg_process_adapter *process_adapter = NULL;
    uint32 fops_kind = HV_DXG_SHARED_FOPS_NONE;
    uint32 reason = HV_DXG_BIND_PIN_REASON_OK;
    uint32 selected = 0;
    int ret = -EINVAL;

    if (snapshot == NULL)
        return -EINVAL;
    memset(snapshot, 0, sizeof(*snapshot));
    if (dxg_fd < 0 || resource_fd < 0 || device == 0 || resource == 0 ||
        allocation == 0 || allocation_count == 0) {
        hvdxg_note_display_bind_pin_diag(
            -EINVAL, HV_DXG_BIND_PIN_REASON_BAD_ARGS, dxg_fd,
            resource_fd, device, resource, allocation, allocation_count,
            NULL, HV_DXG_SHARED_FOPS_NONE, NULL, NULL, NULL, 0);
        return -EINVAL;
    }
    if (current == NULL || current->fdtable == NULL) {
        hvdxg_note_display_bind_pin_diag(
            -EINVAL, HV_DXG_BIND_PIN_REASON_NO_FDTABLE, dxg_fd,
            resource_fd, device, resource, allocation, allocation_count,
            NULL, HV_DXG_SHARED_FOPS_NONE, NULL, NULL, NULL, 0);
        return -EINVAL;
    }

    shared = hvdxg_shared_object_from_fd(resource_fd,
                                         HV_DXG_SHARED_OBJECT_RESOURCE,
                                         &shared_file);
    if (shared == NULL) {
        reason = HV_DXG_BIND_PIN_REASON_NO_SHARED_FD;
        return -EINVAL;
    }
    fops_kind = hvdxg_shared_object_fops_kind(shared_file);
    if (fops_kind != HV_DXG_SHARED_FOPS_RESOURCE ||
        shared->global_share == 0) {
        reason = HV_DXG_BIND_PIN_REASON_BAD_SHARED_FD;
        goto out;
    }
    parent_resource = hvdxg_shared_parent_resource(shared);
    if (parent_resource == NULL || parent_resource->sealed == 0 ||
        parent_resource->shared_records_valid == 0 ||
        parent_resource->sealed_generation == 0) {
        reason = HV_DXG_BIND_PIN_REASON_BAD_PARENT;
        goto out;
    }

    dxg_file = vfs_fdtable_get_file(current->fdtable, dxg_fd);
    if (dxg_file == NULL || dxg_file->ops != &hvdxg_file_ops ||
        dxg_file->private_data == NULL) {
        reason = HV_DXG_BIND_PIN_REASON_BAD_DXG_FD;
        goto out;
    }

    owner = (struct hvdxg_open_state *)dxg_file->private_data;
    opened = hvdxg_owner_find_resource(owner, device, resource);
    if (opened != NULL && opened->allocation_count == allocation_count &&
        opened->allocation_count != 0 &&
        opened->allocation_count <= HV_DXG_ALLOCATION_MAX &&
        opened->allocation_handles[0] == allocation &&
        opened->global_share == shared->global_share) {
        pinned_resource = opened;
        selected = 1;
    } else if (parent_resource->device == device &&
               parent_resource->resource == resource &&
               parent_resource->allocation_count == allocation_count &&
               parent_resource->allocation_count != 0 &&
               parent_resource->allocation_count <=
                   HV_DXG_ALLOCATION_MAX &&
               parent_resource->allocation_handles[0] == allocation &&
               parent_resource->global_share == shared->global_share) {
        pinned_resource = parent_resource;
        selected = 2;
    } else {
        reason = HV_DXG_BIND_PIN_REASON_NO_MATCHING_RESOURCE;
        goto out;
    }

    snapshot->dxg_file_cookie = dxg_file;
    snapshot->resource_file_cookie = shared_file;
    snapshot->dxg_file_pinned = 1;
    snapshot->resource_file_pinned = 1;
    snapshot->kind = shared->kind;
    snapshot->fops_kind = fops_kind;
    snapshot->device = pinned_resource->device;
    snapshot->resource = pinned_resource->resource;
    snapshot->allocation_count = pinned_resource->allocation_count;
    snapshot->first_allocation = pinned_resource->allocation_handles[0];
    snapshot->sealed = parent_resource->sealed;
    snapshot->shared_records_valid = parent_resource->shared_records_valid;
    snapshot->generation = parent_resource->sealed_generation;
    snapshot->host_shared_refs = parent_resource->host_shared_refs;
    if (shared->resource_parent != NULL) {
        snapshot->shared_parent_id = shared->resource_parent->id;
        snapshot->shared_parent_refs = shared->resource_parent->refs;
        snapshot->shared_parent_children = shared->resource_parent->child_count;
        snapshot->shared_parent_fd_refs = shared->resource_parent->fd_refs;
        snapshot->shared_parent_host_nt_refs =
            shared->resource_parent->host_nt_refs;
        snapshot->shared_parent_child_refs =
            shared->resource_parent->child_refs;
        snapshot->shared_parent_global_share =
            shared->resource_parent->global_share;
        snapshot->shared_parent_host_nt_handle =
            shared->resource_parent->host_nt_handle;
    } else {
        snapshot->shared_parent_id = shared->parent_id;
        snapshot->shared_parent_refs = shared->parent_refs;
        snapshot->shared_parent_children = shared->opened_child_count;
        snapshot->shared_parent_fd_refs = shared->fd_refs;
        snapshot->shared_parent_host_nt_refs =
            shared->host_nt_handle != 0 ? 1 : 0;
        snapshot->shared_parent_child_refs = shared->opened_child_count;
        snapshot->shared_parent_global_share = shared->global_share;
        snapshot->shared_parent_host_nt_handle = shared->host_nt_handle;
    }
    snapshot->opened_child_parent_id_match =
        shared->parent_id == snapshot->shared_parent_id;
    snapshot->opened_child_global_share_match =
        opened != NULL && opened->global_share == snapshot->shared_parent_global_share;
    snapshot->opened_child_sealed_generation_match =
        shared->parent_sealed_generation == snapshot->generation;
    snapshot->process = hvdxg_open_host_process(owner);
    snapshot->process_tgid =
        owner->process_state != NULL ? owner->process_state->tgid : 0;
    snapshot->process_generation = hvdxg_open_process_generation(owner);
    snapshot->process_refs = hvdxg_open_process_refs(owner);
    if (owner->process_state != NULL)
        process_adapter = hvdxg_process_find_adapter(owner->process_state,
                                                     hvdxg.host_adapter_handle);
    if (process_adapter != NULL) {
        snapshot->process_adapter_generation = process_adapter->generation;
        snapshot->process_adapter_refs = process_adapter->refs;
    }
    snapshot->device_hmgr_index_unique_valid =
        hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_DEVICE,
                                             device);
    snapshot->resource_hmgr_index_unique_valid =
        hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_RESOURCE,
                                             resource);
    snapshot->allocation_hmgr_index_unique_valid =
        hvdxg_hmgr_handle_index_unique_valid(owner, HV_DXG_OBJECT_ALLOCATION,
                                             allocation);
    snapshot->hmgr_index_unique_valid =
        snapshot->device_hmgr_index_unique_valid &&
        snapshot->resource_hmgr_index_unique_valid &&
        snapshot->allocation_hmgr_index_unique_valid;
    snapshot->device_object_ref_active =
        hvdxg_hmgr_object_ref_active(owner, HV_DXG_OBJECT_DEVICE, device);
    snapshot->resource_object_ref_active =
        hvdxg_hmgr_object_ref_active(owner, HV_DXG_OBJECT_RESOURCE, resource);
    snapshot->allocation_object_ref_active =
        hvdxg_hmgr_object_ref_active(owner, HV_DXG_OBJECT_ALLOCATION,
                                   allocation);
    snapshot->shared_parent_snapshot_valid =
        snapshot->shared_parent_id != 0 &&
        snapshot->shared_parent_refs != 0 &&
        snapshot->shared_parent_fd_refs != 0 &&
        snapshot->shared_parent_global_share != 0 &&
        snapshot->shared_parent_host_nt_handle != 0;
    snapshot->opened_child_snapshot_valid =
        opened != NULL &&
        snapshot->shared_parent_id != 0 &&
        snapshot->shared_parent_children != 0 &&
        snapshot->opened_child_parent_id_match != 0 &&
        snapshot->opened_child_global_share_match != 0 &&
        snapshot->opened_child_sealed_generation_match != 0;
    ret = 0;
    reason = HV_DXG_BIND_PIN_REASON_OK;

out:
    hvdxg_note_display_bind_pin_diag(
        ret, reason, dxg_fd, resource_fd, device, resource, allocation,
        allocation_count, shared, fops_kind, parent_resource, owner, opened,
        selected);
    if (ret != 0) {
        if (dxg_file != NULL)
            vfs_fput(dxg_file);
        if (shared_file != NULL)
            vfs_fput(shared_file);
    }
    return ret;
}

void hyperv_dxg_display_bind_unpin(
    struct hyperv_dxg_display_bind_pin_snapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    if (snapshot->dxg_file_cookie != NULL) {
        vfs_fput((struct vfs_file *)snapshot->dxg_file_cookie);
        snapshot->dxg_file_cookie = NULL;
        snapshot->dxg_file_pinned = 0;
    }
    if (snapshot->resource_file_cookie != NULL) {
        vfs_fput((struct vfs_file *)snapshot->resource_file_cookie);
        snapshot->resource_file_cookie = NULL;
        snapshot->resource_file_pinned = 0;
    }
}

static uint64
hvdxg_display_bind_missing_required_metadata(
    const struct hyperv_dxg_display_bind_request *bind)
{
    uint64 expected;
    uint64 missing = 0;

    if (bind == NULL)
        return FB_GPU_DXG_PRESENT_META_DEVICE |
               FB_GPU_DXG_PRESENT_META_RESOURCE |
               FB_GPU_DXG_PRESENT_META_ALLOCATION |
               FB_GPU_DXG_PRESENT_META_DIMENSIONS |
               FB_GPU_DXG_PRESENT_META_FORMAT |
               FB_GPU_DXG_PRESENT_META_MODIFIER |
               FB_GPU_DXG_PRESENT_META_ADAPTER_LUID |
               FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
               FB_GPU_DXG_PRESENT_META_FENCE_VALUE;

    expected = FB_GPU_DXG_PRESENT_META_DEVICE |
               FB_GPU_DXG_PRESENT_META_RESOURCE |
               FB_GPU_DXG_PRESENT_META_ALLOCATION |
               FB_GPU_DXG_PRESENT_META_DIMENSIONS |
               FB_GPU_DXG_PRESENT_META_FORMAT |
               FB_GPU_DXG_PRESENT_META_MODIFIER |
               FB_GPU_DXG_PRESENT_META_ADAPTER_LUID;
    if ((bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0)
        expected |= FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
                    FB_GPU_DXG_PRESENT_META_FENCE_VALUE;

    missing |= expected & ~bind->required_metadata;
    if (bind->device == 0)
        missing |= FB_GPU_DXG_PRESENT_META_DEVICE;
    if (bind->resource == 0)
        missing |= FB_GPU_DXG_PRESENT_META_RESOURCE;
    if (bind->allocation == 0 || bind->allocation_count == 0)
        missing |= FB_GPU_DXG_PRESENT_META_ALLOCATION;
    if (bind->width == 0 || bind->height == 0 || bind->pitch == 0)
        missing |= FB_GPU_DXG_PRESENT_META_DIMENSIONS;
    if (bind->format == 0)
        missing |= FB_GPU_DXG_PRESENT_META_FORMAT;
    if (bind->adapter_luid_low == 0 && bind->adapter_luid_high == 0)
        missing |= FB_GPU_DXG_PRESENT_META_ADAPTER_LUID;
    if ((bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) != 0) {
        if (bind->sync_object == 0)
            missing |= FB_GPU_DXG_PRESENT_META_SYNC_OBJECT;
        if (bind->fence_value == 0)
            missing |= FB_GPU_DXG_PRESENT_META_FENCE_VALUE;
    }
    return missing;
}

int hyperv_dxg_display_bind_submit_failclosed(
    const struct hyperv_dxg_display_bind_request *bind,
    struct hyperv_dxg_display_bind_result *result)
{
    uint64 block_reason;
    uint64 missing_metadata;
    int pin_valid;

    if (bind == NULL || result == NULL)
        return -EINVAL;

    memset(result, 0, sizeof(*result));
    block_reason = bind->block_reason |
                   FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
                   FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND |
                   FB_GPU_DXG_PRESENT_BLOCK_WSL_ENUM_ONLY |
                   FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION;
    result->transport = FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE;
    result->operation = FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND;
    result->completion_source = FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
    result->present_id = 0;
    result->completed_id = 0;
    result->source_generation = bind->source_generation;
    result->resource_generation = bind->resource_generation;
    result->host_abi_present = 0;
    result->sender_present = 0;
    result->completion_present = 0;
    result->no_host_abi = 1;
    result->no_sender = 1;
    result->no_completion = 1;
    result->publication_attempted = 1;
    result->publish_before_send = 0;
    result->transport_pending_id = 0;
    result->command_id = 0;
    result->transaction_id = 0;
    result->channel = 0;
    result->completion_demux_registered = 0;
    result->transport_source = FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE;
    result->host_saw_packet = 0;
    result->wsl_presenthistory_completion_credit = 0;
    result->resolved_or_cancelled = 1;
    result->refs_released = 1;
    result->no_host_abi_cancelled = 1;
    result->no_host_abi_refs_released = 1;
    result->pending_owner_generation =
        bind->pin_valid ? bind->pin.process_generation : 0;
    result->pending_source_generation = bind->source_generation;
    result->pending_resource_generation = bind->resource_generation;
    result->pending_dxgprocess_generation =
        bind->pin_valid ? bind->pin.process_generation : 0;
    result->pending_process_adapter_generation =
        bind->pin_valid ? bind->pin.process_adapter_generation : 0;
    result->pending_hmgr_index_unique_valid =
        bind->pin_valid ? bind->pin.hmgr_index_unique_valid : 0;
    result->pending_process_namespace_valid =
        bind->pin_valid && bind->pin.process_tgid != 0 &&
        bind->pin.process_generation != 0 &&
        bind->pin.process_adapter_generation != 0;
    result->pending_device_hmgr_index_unique_valid =
        bind->pin_valid ? bind->pin.device_hmgr_index_unique_valid : 0;
    result->pending_resource_hmgr_index_unique_valid =
        bind->pin_valid ? bind->pin.resource_hmgr_index_unique_valid : 0;
    result->pending_allocation_hmgr_index_unique_valid =
        bind->pin_valid ? bind->pin.allocation_hmgr_index_unique_valid : 0;
    result->pending_device_object_ref_active =
        bind->pin_valid ? bind->pin.device_object_ref_active : 0;
    result->pending_resource_object_ref_active =
        bind->pin_valid ? bind->pin.resource_object_ref_active : 0;
    result->pending_allocation_object_ref_active =
        bind->pin_valid ? bind->pin.allocation_object_ref_active : 0;
    result->pending_shared_parent_id =
        bind->pin_valid ? bind->pin.shared_parent_id : 0;
    result->pending_shared_parent_refs =
        bind->pin_valid ? bind->pin.shared_parent_refs : 0;
    result->pending_shared_parent_children =
        bind->pin_valid ? bind->pin.shared_parent_children : 0;
    result->pending_shared_parent_fd_refs =
        bind->pin_valid ? bind->pin.shared_parent_fd_refs : 0;
    result->pending_shared_parent_host_nt_refs =
        bind->pin_valid ? bind->pin.shared_parent_host_nt_refs : 0;
    result->pending_shared_parent_child_refs =
        bind->pin_valid ? bind->pin.shared_parent_child_refs : 0;
    result->pending_shared_parent_global_share =
        bind->pin_valid ? bind->pin.shared_parent_global_share : 0;
    result->pending_shared_parent_host_nt_handle =
        bind->pin_valid ? bind->pin.shared_parent_host_nt_handle : 0;
    result->pending_opened_child_parent_id_match =
        bind->pin_valid ? bind->pin.opened_child_parent_id_match : 0;
    result->pending_opened_child_global_share_match =
        bind->pin_valid ? bind->pin.opened_child_global_share_match : 0;
    result->pending_opened_child_sealed_generation_match =
        bind->pin_valid ? bind->pin.opened_child_sealed_generation_match : 0;
    result->pending_shared_parent_snapshot_valid =
        bind->pin_valid ? bind->pin.shared_parent_snapshot_valid : 0;
    result->pending_opened_child_snapshot_valid =
        bind->pin_valid ? bind->pin.opened_child_snapshot_valid : 0;
    result->pending_shared_parent_global_share_match =
        result->pending_opened_child_snapshot_valid;
    if (bind->pin_valid && bind->pin.dxg_file_cookie != NULL &&
        bind->sync_object != 0) {
        struct hvdxg_open_state *owner =
            (struct hvdxg_open_state *)
                ((struct vfs_file *)bind->pin.dxg_file_cookie)->private_data;
        uint64 fence_cpu_va;
        uint64 fence_kva;
        uint64 fence_gpu_va;

        result->pending_syncobject_object_ref_active =
            hvdxg_hmgr_object_ref_active(owner, HV_DXG_OBJECT_SYNC,
                                       bind->sync_object);
        result->pending_syncobject_shared_owner_present =
            hvdxg_owner_sync_global_shared(owner, bind->sync_object) != 0;
        result->pending_syncobject_monitored_fence =
            hvdxg_owner_sync_is_monitored(owner, bind->sync_object) ||
            hvdxg_owner_sync_is_monitor_fence_handle(owner,
                                                     bind->sync_object);
        result->pending_syncobject_fence_value = bind->fence_value;
        fence_cpu_va = hvdxg_owner_sync_fence_cpu_va(owner,
                                                     bind->sync_object);
        fence_kva = hvdxg_owner_sync_fence_kva(owner, bind->sync_object);
        fence_gpu_va = hvdxg_owner_sync_fence_gpu_va(owner,
                                                     bind->sync_object);
        result->pending_syncobject_fence_cpu_va_present =
            fence_cpu_va != 0;
        result->pending_syncobject_fence_gpu_va_present = fence_gpu_va != 0;
        result->pending_syncobject_fence_kva_present = fence_kva != 0;
        result->pending_syncobject_fence_gpu_va_alias_gap =
            fence_kva != 0 && fence_gpu_va == 0;
        result->pending_syncobject_real_fence_gpu_va_present =
            fence_gpu_va != 0;
        result->pending_syncobject_fence_gpu_va_source =
            hvdxg_owner_sync_fence_gpu_va_source(owner, bind->sync_object);
        result->pending_syncobject_fence_map_size =
            hvdxg_owner_sync_fence_map_size(owner, bind->sync_object);
    }
    result->pending_owner_close_cancelled = 0;
    missing_metadata =
        hvdxg_display_bind_missing_required_metadata(bind);
    result->request_missing_metadata = missing_metadata;
    result->request_metadata_complete = missing_metadata == 0;
    result->request_sync_metadata_complete =
        (bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 ||
        (missing_metadata & (FB_GPU_DXG_PRESENT_META_SYNC_OBJECT |
                             FB_GPU_DXG_PRESENT_META_FENCE_VALUE)) == 0;

    pin_valid = bind->pin_valid &&
                bind->pin.dxg_file_pinned != 0 &&
                bind->pin.resource_file_pinned != 0 &&
                bind->pin.sealed != 0 &&
                bind->pin.shared_records_valid != 0 &&
                bind->pin.generation != 0 &&
                bind->pin.device == bind->device &&
                bind->pin.resource == bind->resource &&
                bind->pin.first_allocation == bind->allocation &&
                bind->pin.allocation_count == bind->allocation_count &&
                bind->pin.process != 0 &&
                bind->pin.process_generation != 0 &&
                bind->pin.process_adapter_generation != 0 &&
                bind->pin.hmgr_index_unique_valid != 0 &&
                bind->pin.device_object_ref_active != 0 &&
                bind->pin.resource_object_ref_active != 0 &&
                bind->pin.allocation_object_ref_active != 0 &&
                bind->pin.shared_parent_id != 0 &&
                bind->pin.shared_parent_refs != 0 &&
                bind->pin.shared_parent_children != 0 &&
                bind->pin.shared_parent_fd_refs != 0 &&
                bind->pin.shared_parent_global_share != 0 &&
                bind->pin.shared_parent_host_nt_handle != 0 &&
                bind->pin.opened_child_parent_id_match != 0 &&
                bind->pin.opened_child_global_share_match != 0 &&
                bind->pin.opened_child_sealed_generation_match != 0 &&
                bind->pin.shared_parent_snapshot_valid != 0 &&
                bind->pin.opened_child_snapshot_valid != 0 &&
                ((bind->flags & FB_GPU_DXG_PRESENT_F_WAIT_SYNC) == 0 ||
                 result->pending_syncobject_object_ref_active != 0);
    if (pin_valid)
        result->pin_revalidated = 1;
    else
        block_reason |= FB_GPU_DXG_PRESENT_BLOCK_RESOURCE_FD_UNVERIFIED;

    result->preflight_ready = (missing_metadata == 0 && pin_valid);
    result->send_attempts = 0;
    result->send_blocked_no_host_abi =
        result->preflight_ready != 0 ? 1 : 0;
    result->completion_demux_attempts = 0;
    result->completion_demux_blocked_no_contract =
        result->preflight_ready != 0 ? 1 : 0;
    /*
     * Define the narrow display-bind packet boundary without emitting it.
     * The wire format a future GPU-P/DDA sender must marshal is now declared
     * (packet_boundary_defined=1), but the fail-closed provider never puts a
     * packet on the wire: request/completion stay unsent and host_saw_packet
     * remains 0, so no native-present or OpenGL-submit credit is granted.
     */
    result->packet_boundary_defined = 1;
    result->packet_abi_version = HV_DXG_DISPLAY_BIND_PACKET_ABI_VERSION;
    result->packet_request_command_id =
        HV_DXG_DISPLAY_BIND_PACKET_CMD_REQUEST;
    result->packet_completion_command_id =
        HV_DXG_DISPLAY_BIND_PACKET_CMD_COMPLETION;
    result->packet_request_size =
        (uint32)sizeof(struct hyperv_dxg_display_bind_packet_request);
    result->packet_completion_size =
        (uint32)sizeof(struct hyperv_dxg_display_bind_packet_completion);
    result->packet_request_sent = 0;
    result->packet_completion_seen = 0;
    result->block_reason = block_reason;
    result->status = EOPNOTSUPP;
    return -EOPNOTSUPP;
}

int hyperv_dxg_display_bind_submit(
    const struct hyperv_dxg_display_bind_request *bind,
    struct hyperv_dxg_display_bind_result *result)
{
    return hyperv_dxg_display_bind_submit_failclosed(bind, result);
}

int hyperv_dxg_display_bind_cancel_failclosed(
    uint64 transport_pending_id, uint64 source_generation,
    uint64 resource_generation, uint64 reason,
    struct hyperv_dxg_display_bind_result *result)
{
    (void)transport_pending_id;
    if (result == NULL)
        return -EINVAL;
    memset(result, 0, sizeof(*result));
    result->status = ESTALE;
    result->transport = FB_GPU_DXG_PRESENT_GPUP_DDA_TRANSPORT_NONE;
    result->operation = FB_GPU_DXG_PRESENT_GPUP_DDA_OP_SCANOUT_BIND;
    result->completion_source = FB_GPU_DXG_PRESENT_COMPLETION_DISPLAY;
    result->source_generation = source_generation;
    result->resource_generation = resource_generation;
    result->block_reason = reason |
                           FB_GPU_DXG_PRESENT_BLOCK_NO_TRANSPORT |
                           FB_GPU_DXG_PRESENT_BLOCK_DXG_NO_DISPLAY_BIND |
                           FB_GPU_DXG_PRESENT_BLOCK_NO_COMPLETION;
    result->no_host_abi = 1;
    result->no_sender = 1;
    result->no_completion = 1;
    result->resolved_or_cancelled = 1;
    result->refs_released = 1;
    result->no_host_abi_cancelled = 1;
    result->no_host_abi_refs_released = 1;
    result->pending_owner_close_cancelled = 1;
    result->transport_source = FB_GPU_DXG_DISPLAY_BIND_SOURCE_NONE;
    result->wsl_presenthistory_completion_credit = 0;
    return -EOPNOTSUPP;
}

int hyperv_dxg_display_bind_cancel(
    uint64 transport_pending_id, uint64 source_generation,
    uint64 resource_generation, uint64 reason,
    struct hyperv_dxg_display_bind_result *result)
{
    return hyperv_dxg_display_bind_cancel_failclosed(
        transport_pending_id, source_generation, resource_generation, reason,
        result);
}

static int hvdxg_sync_file_release(struct vfs_inode *ip,
                                   struct vfs_file *file)
{
    struct hvdxg_sync_file_object *sync_file =
        file != NULL ?
        (struct hvdxg_sync_file_object *)file->private_data : NULL;

    (void)ip;
    if (sync_file != NULL) {
        hvdxg.syncfile_release_count++;
        if (hvdxg.syncfile_live_count != 0)
            hvdxg.syncfile_live_count--;
        if (sync_file->event_id != 0) {
            hvdxg_remove_host_event(sync_file->event_id);
            hvdxg.syncfile_release_event_removed++;
        }
        if (sync_file->host_nt_handle != 0) {
            (void)hvdxg_release_nt_shared_object_ref(
                HV_DXG_SHARED_OBJECT_SYNC, sync_file->cache_process,
                sync_file->cache_object != 0 ? sync_file->cache_object :
                sync_file->sync_object,
                sync_file->host_nt_handle);
            hvdxg.syncfile_release_nt_released++;
        }
        kvfree(sync_file);
        file->private_data = NULL;
    }
    return 0;
}

static int hvdxg_sync_file_stat(struct vfs_file *file, struct stat *st)
{
    if (file == NULL || file->private_data == NULL || st == NULL)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_ino = 0x64786703ULL;
    st->st_blksize = 4096;
    return 0;
}

static ssize_t hvdxg_sync_file_readlink(struct vfs_file *file, char *buf,
                                        size_t buflen)
{
    if (file == NULL || file->private_data == NULL || buf == NULL)
        return -EINVAL;
    return snprintf(buf, buflen, "anon_inode:sync_file");
}

static struct vfs_file_ops hvdxg_sync_file_ops = {
    .stat = hvdxg_sync_file_stat,
    .readlink = hvdxg_sync_file_readlink,
    .release = hvdxg_sync_file_release,
};

static struct hvdxg_sync_file_object *hvdxg_sync_file_from_fd(
    int fd, struct vfs_file **file_out)
{
    struct vfs_file *f;

    if (file_out != NULL)
        *file_out = NULL;
    if (fd < 0 || current == NULL || current->fdtable == NULL)
        return NULL;
    f = vfs_fdtable_get_file(current->fdtable, fd);
    if (f == NULL)
        return NULL;
    if (f->ops != &hvdxg_sync_file_ops || f->private_data == NULL) {
        vfs_fput(f);
        return NULL;
    }
    if (file_out != NULL)
        *file_out = f;
    else
        vfs_fput(f);
    return (struct hvdxg_sync_file_object *)f->private_data;
}

static int hvdxg_open_sync_file_on_device(
    struct hvdxg_open_state *owner, uint32 device,
    struct hvdxg_sync_file_object *sync_file,
    struct d3dddi_synchronizationobject_flags flags, uint32 engine_affinity,
    int map_fence_va, struct hvdxg_d3dkmthandle *sync_out, uint64 *cpu_va_out,
    uint64 *gpu_va_out, uint64 *kva_out)
{
    struct hvdxg_command_opensyncobject open;
    struct hvdxg_command_opensyncobject_return result;
    struct hvdxg_d3dkmthandle open_process;
    uint32 actual_len = 0;
    uint64 fence_pa = 0;
    uint64 fence_kva = 0;
    int ret;

    if (sync_out != NULL)
        sync_out->v = 0;
    if (cpu_va_out != NULL)
        *cpu_va_out = 0;
    if (gpu_va_out != NULL)
        *gpu_va_out = 0;
    if (kva_out != NULL)
        *kva_out = 0;
    if (owner == NULL || sync_file == NULL || device == 0 ||
        sync_file->global_share == 0 || !hvdxg_owner_has_device(owner, device))
        return -EINVAL;

    memset(&open, 0, sizeof(open));
    memset(&result, 0, sizeof(result));
    open_process = hvdxg_owner_bound_process_handle(owner);
    hvdxg_command_vm_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENSYNCOBJECT);
    open.hdr.process = open_process;
    open.device.v = device;
    open.global_sync_object.v = sync_file->global_share;
    open.flags = flags;
    open.engine_affinity = engine_affinity;

    hvdxg.opensync_ioctl_count++;
    hvdxg.opensync_last_cmd_len = 0;
    hvdxg.opensync_last_wire_len = 0;
    hvdxg.opensync_last_ext = 0;
    hvdxg.opensync_last_ext_offset = 0;
    hvdxg.opensync_last_result_len = sizeof(result);
    hvdxg.opensync_last_actual_len = 0;
    hvdxg.opensync_last_ret = -ENOTSUP;
    hvdxg.opensync_last_status = 0;
    hvdxg.opensync_last_process = open_process.v;
    hvdxg.opensync_last_device = device;
    hvdxg.opensync_last_device_host =
        hvdxg_owner_host_object_handle(owner, HV_DXG_OBJECT_DEVICE,
                                       device, NULL);
    hvdxg.opensync_last_device_owner = hvdxg_open_host_process(owner);
    hvdxg.opensync_last_device_owner_generation =
        hvdxg_open_process_generation(owner);
    hvdxg.opensync_last_device_generation = 0;
    hvdxg.opensync_last_global = sync_file->global_share;
    hvdxg.opensync_last_input_nt = 0;
    hvdxg.opensync_last_host_nt = sync_file->host_nt_handle;
    hvdxg.opensync_last_object = sync_file->sync_object;
    hvdxg.opensync_last_cache_object = sync_file->cache_object;
    hvdxg.opensync_last_source_device = sync_file->device;
    hvdxg.opensync_last_source_device_host =
        hvdxg_owner_host_object_handle(owner, HV_DXG_OBJECT_DEVICE,
                                       sync_file->device, NULL);
    hvdxg.opensync_last_source_owner = sync_file->cache_process;
    hvdxg.opensync_last_source_owner_generation = 0;
    hvdxg.opensync_last_source_flags = sync_file->sync_flags;
    hvdxg.opensync_last_same_device =
        device != 0 && sync_file->device == device;
    hvdxg.opensync_last_adapter_match =
        hvdxg_luid_nonzero(hvdxg.adapter_luid) &&
        hvdxg_luid_nonzero(hvdxg.host_adapter_luid) ? 1 : 0;
    hvdxg.opensync_last_adapter_low = hvdxg.adapter_luid.a;
    hvdxg.opensync_last_adapter_high = hvdxg.adapter_luid.b;
    hvdxg.opensync_last_host_adapter_low = hvdxg.host_adapter_luid.a;
    hvdxg.opensync_last_host_adapter_high = hvdxg.host_adapter_luid.b;
    hvdxg.opensync_last_flags = flags.value;
    hvdxg.opensync_last_wire_flags = flags.value;
    hvdxg.opensync_last_forced_flags = 0;
    hvdxg.opensync_last_fops_kind = 0;
    hvdxg.opensync_last_sync_type = sync_file->sync_type;
    hvdxg.opensync_last_result_sync = 0;
    hvdxg.opensync_last_gpu_va = 0;
    hvdxg.opensync_last_cpu_pa = 0;
    hvdxg.opensync_last_fd_kind = HV_DXG_SHARED_OBJECT_SYNC;
    hvdxg.opensync_last_fd_refs = hvdxg_ntshared_cache_refs(
        HV_DXG_SHARED_OBJECT_SYNC, sync_file->cache_process,
        sync_file->cache_object != 0 ? sync_file->cache_object :
        sync_file->sync_object,
        sync_file->host_nt_handle);
    hvdxg.opensync_last_gate = 3;
    hvdxg.opensync_last_current_tgid =
        current != NULL ? (uint64)thread_tgid(current) : 0;
    hvdxg.opensync_last_owner_tgid =
        owner != NULL && owner->process_state != NULL ?
        owner->process_state->tgid : 0;
    hvdxg.opensync_last_owner_generation =
        owner != NULL && owner->process_state != NULL ?
        owner->process_state->generation : 0;
    hvdxg.opensync_last_namespace_mismatch =
        hvdxg.opensync_last_owner_tgid != 0 &&
        hvdxg.opensync_last_current_tgid != 0 &&
        hvdxg.opensync_last_owner_tgid != hvdxg.opensync_last_current_tgid;

    ret = hvdxg_send_sync_global(&open, sizeof(open), &result,
                                 sizeof(result), &actual_len);
    hvdxg.opensync_last_cmd_len = hvdxg.global_send_last_cmd_len;
    hvdxg.opensync_last_wire_len = hvdxg.global_send_last_wire_len;
    hvdxg.opensync_last_ext = hvdxg.global_send_last_ext;
    hvdxg.opensync_last_ext_offset = hvdxg.global_send_last_ext_offset;
    hvdxg.opensync_last_actual_len = actual_len;
    hvdxg.opensync_last_status = result.status.v;
    hvdxg.opensync_last_result_sync = result.sync_object.v;
    hvdxg.opensync_last_gpu_va = result.gpu_virtual_address;
    hvdxg.opensync_last_cpu_pa = result.guest_cpu_physical_address;
    if (ret == 0 && actual_len < sizeof(result))
        ret = -EOVERFLOW;
    if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(result.status);
    if (ret == 0 && map_fence_va &&
        sync_file->sync_type == _D3DDDI_MONITORED_FENCE) {
        uint64 cpu_va;

        cpu_va = hvdxg_map_iospace_user_canonical(
            HV_DXG_FENCE_SOURCE_OPEN_SYNC,
            result.guest_cpu_physical_address, PGSIZE,
            0, &fence_pa, NULL, 1);
        fence_kva = hvdxg_map_iospace_kernel_canonical(
            HV_DXG_FENCE_SOURCE_OPEN_SYNC,
            result.guest_cpu_physical_address, PGSIZE, fence_pa, 1);
        if (cpu_va == 0)
            ret = -ENOMEM;
        if (cpu_va_out != NULL)
            *cpu_va_out = cpu_va;
    }
    if (ret == 0) {
        if (sync_out != NULL)
            *sync_out = result.sync_object;
        if (gpu_va_out != NULL)
            *gpu_va_out = result.gpu_virtual_address;
        if (kva_out != NULL)
            *kva_out = fence_kva;
    }
    hvdxg.opensync_last_ret = ret;
    hvdxg.opensync_last_gate = ret == 0 ? 5 : 4;
    return ret;
}

static int hvdxg_destroy_syncobject_host_for_syncfile(
    struct hvdxg_open_state *owner, uint32 device, uint32 sync,
    uint32 type, uint32 flags, uint32 global_shared)
{
    struct hvdxg_command_destroysyncobject destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (sync == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vm_init(&destroy.hdr,
                          HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT);
    destroy.hdr.process = hvdxg_owner_bound_process_handle(owner);
    destroy.sync_object.v = sync;
    ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                 sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroysync_last_handle = sync;
    hvdxg.destroysync_last_device = device;
    hvdxg.destroysync_last_type = type;
    hvdxg.destroysync_last_flags = flags;
    hvdxg.destroysync_last_global = global_shared;
    hvdxg.destroysync_last_monitor_fence = 0;
    hvdxg.destroysync_last_cmd_len = hvdxg.global_send_last_cmd_len;
    hvdxg.destroysync_last_wire_len = hvdxg.global_send_last_wire_len;
    hvdxg.destroysync_last_ext = hvdxg.global_send_last_ext;
    hvdxg.destroysync_last_ext_offset = hvdxg.global_send_last_ext_offset;
    hvdxg.destroysync_last_len = actual_len;
    hvdxg.destroysync_last_status = status.v;
    hvdxg.destroysync_last_ret = ret;
    return ret;
}

static int hvdxg_share_object_with_host(uint32 process, uint32 device,
                                        uint32 object, uint64 reserved,
                                        uint64 *nt_handle,
                                        uint32 *actual_len_out,
                                        int32 *status_out)
{
    struct hvdxg_command_shareobjectwithhost share;
    struct hvdxg_command_shareobjectwithhost_return result;
    uint32 actual_len = 0;
    int ret;

    if (nt_handle != NULL)
        *nt_handle = 0;
    if (actual_len_out != NULL)
        *actual_len_out = 0;
    if (status_out != NULL)
        *status_out = 0;
    if (process == 0 || device == 0 || object == 0)
        return -EINVAL;
    hvdxg.shareobject_last_cmd_len = sizeof(share);
    hvdxg.shareobject_last_wire_len = 0;
    hvdxg.shareobject_last_ext = 0;
    hvdxg.shareobject_last_ext_offset = 0;
    hvdxg.shareobject_last_device_offset =
        offsetof(struct hvdxg_command_shareobjectwithhost, device_handle);
    hvdxg.shareobject_last_object_offset =
        offsetof(struct hvdxg_command_shareobjectwithhost, object_handle);
    hvdxg.shareobject_last_result_len = sizeof(result);
    hvdxg.shareobject_last_head_len = 0;
    memset(hvdxg.shareobject_last_head, 0,
           sizeof(hvdxg.shareobject_last_head));
    hvdxg.shareobject_last_completion_type = 0;
    hvdxg.shareobject_last_completion_len = 0;
    memset(hvdxg.shareobject_last_completion_prefix, 0,
           sizeof(hvdxg.shareobject_last_completion_prefix));
    hvdxg.shareobject_last_process = process;
    hvdxg.shareobject_last_device = device;
    hvdxg.shareobject_last_object = object;
    hvdxg.shareobject_last_reserved = 0;
    memset(&share, 0, sizeof(share));
    memset(&result, 0, sizeof(result));
    hvdxg_command_vm_init(&share.hdr,
                          HV_DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST);
    share.hdr.process.v = process;
    share.device_handle.v = device;
    share.object_handle.v = object;
    (void)reserved;
    share.reserved = 0;
    hvdxg_note_shareobject_wire(&share, sizeof(share), sizeof(result));
    ret = hvdxg_send_sync_global(&share, sizeof(share), &result,
                                 sizeof(result), &actual_len);
    hvdxg_snapshot_last_completion(&hvdxg.shareobject_last_completion_type,
                                   &hvdxg.shareobject_last_completion_len,
                                   hvdxg.shareobject_last_completion_prefix);
    if (actual_len_out != NULL)
        *actual_len_out = actual_len;
    if (actual_len >= sizeof(result) && status_out != NULL)
        *status_out = result.status.v;
    if (ret == 0 && actual_len < sizeof(result))
        ret = -EOVERFLOW;
    if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(result.status);
    if (ret == 0 && nt_handle != NULL)
        *nt_handle = result.vail_nt_handle;
    return ret;
}

static void hvdxg_share_object_with_host_diagnostic(uint32 process,
                                                   uint32 device,
                                                   uint32 object,
                                                   uint32 kind,
                                                   uint32 reason)
{
    uint64 nt_handle = 0;
    uint32 actual_len = 0;
    int32 status = 0;
    int ret;

    hvdxg.shareobject_diag_attempted = 0;
    hvdxg.shareobject_diag_valid_nt = 0;
    hvdxg.shareobject_diag_kind = kind;
    hvdxg.shareobject_diag_reason = reason;
    if (process == 0 || device == 0 || object == 0)
        return;
    hvdxg.shareobject_diag_attempted = 1;
    ret = hvdxg_share_object_with_host(process, device, object, 0,
                                       &nt_handle, &actual_len, &status);
    hvdxg.shareobject_last_len = actual_len;
    hvdxg.shareobject_last_ret = ret;
    hvdxg.shareobject_last_status = status;
    hvdxg.shareobject_last_nt_handle = nt_handle;
    hvdxg.shareobject_diag_valid_nt =
        ret == 0 && actual_len >=
        sizeof(struct hvdxg_command_shareobjectwithhost_return) &&
        nt_handle != 0 ? 1 : 0;
}

static int hvdxg_owner_has_gpuva(struct hvdxg_open_state *owner,
                                 uint32 adapter, uint64 base)
{
    struct hvdxg_object_entry *obj;

    if (owner == NULL || base == 0)
        return 0;
    obj = hvdxg_owner_find_object(owner, HV_DXG_OBJECT_GPUVA, base);
    if (obj == NULL) {
        hvdxg.object_table_denied++;
        return 0;
    }
    return adapter == 0 || obj->device == adapter;
}

static int hvdxg_owner_has_gpuva_alias(struct hvdxg_open_state *owner,
                                       uint32 adapter, uint32 host_adapter,
                                       uint64 base)
{
    if (hvdxg_owner_has_gpuva(owner, adapter, base))
        return 1;
    if (host_adapter != 0 && host_adapter != adapter)
        return hvdxg_owner_has_gpuva(owner, host_adapter, base);
    return 0;
}

static int hvdxg_owner_gpuva_contains(struct hvdxg_open_state *owner,
                                      uint32 adapter, uint64 base,
                                      uint64 size)
{
    uint64 end;

    if (owner == NULL || base == 0 || size == 0)
        return 0;
    end = base + size;
    if (end < base)
        return 0;
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        uint64 va_base = owner->gpuvas[i].base;
        uint64 va_end = va_base + owner->gpuvas[i].size;

        if (va_end < va_base)
            continue;
        if ((adapter == 0 || owner->gpuvas[i].adapter == adapter) &&
            base >= va_base && end <= va_end)
            return 1;
    }
    return 0;
}

static int hvdxg_owner_gpuva_contains_alias(struct hvdxg_open_state *owner,
                                           uint32 adapter,
                                           uint32 host_adapter, uint64 base,
                                           uint64 size)
{
    if (hvdxg_owner_gpuva_contains(owner, adapter, base, size))
        return 1;
    if (host_adapter != 0 && host_adapter != adapter)
        return hvdxg_owner_gpuva_contains(owner, host_adapter, base, size);
    return 0;
}

static void hvdxg_track_gpuva(struct hvdxg_open_state *owner, uint32 adapter,
                              uint64 base, uint64 size, uint64 fence_value,
                              uint64 fence_cpu_pa)
{
    if (owner == NULL || adapter == 0 || base == 0 || size == 0)
        return;
    (void)hvdxg_track_object(owner, HV_DXG_OBJECT_GPUVA, base, size,
                             adapter);
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        if (owner->gpuvas[i].adapter == adapter &&
            owner->gpuvas[i].base == base) {
            owner->gpuvas[i].size = size;
            owner->gpuvas[i].fence_value = fence_value;
            owner->gpuvas[i].fence_cpu_pa = fence_cpu_pa;
            return;
        }
    }
    if (hvdxg_grow_table((void **)&owner->gpuvas,
                         &owner->gpuva_capacity,
                         owner->gpuva_count + 1,
                         sizeof(owner->gpuvas[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        struct hvdxg_tracked_gpuva *slot =
            &owner->gpuvas[owner->gpuva_count++];

        memset(slot, 0, sizeof(*slot));
        slot->adapter = adapter;
        slot->base = base;
        slot->size = size;
        slot->fence_value = fence_value;
        slot->fence_cpu_pa = fence_cpu_pa;
        if (owner->gpuva_count > hvdxg.track_gpuva_max)
            hvdxg.track_gpuva_max = owner->gpuva_count;
    } else {
        hvdxg.track_gpuva_drops++;
    }
}

static void hvdxg_untrack_gpuva(struct hvdxg_open_state *owner,
                                uint64 base)
{
    if (owner == NULL || base == 0)
        return;
    hvdxg_untrack_object(owner, HV_DXG_OBJECT_GPUVA, base);
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        if (owner->gpuvas[i].base == base) {
            owner->gpuvas[i] = owner->gpuvas[owner->gpuva_count - 1];
            memset(&owner->gpuvas[owner->gpuva_count - 1], 0,
                   sizeof(owner->gpuvas[0]));
            owner->gpuva_count--;
            return;
        }
    }
}

static int hvdxg_destroy_device_host_process(uint32 process, uint32 device)
{
    struct hvdxg_command_destroydevice destroy;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    int ret;

    if (device == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    actual_len = 0;
    process_handle.v = process;
    if (process_handle.v == 0)
        process_handle = hvdxg.dxg_process;
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYDEVICE,
                                    process_handle);
    destroy.device.v = device;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (actual_len >= sizeof(status))
        hvdxg.destroydevice_last_status = status.v;
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroydevice_last_len = actual_len;
    hvdxg.destroydevice_last_ret = ret;
    return ret;
}

static int hvdxg_destroy_device_host(uint32 device)
{
    return hvdxg_destroy_device_host_process(hvdxg.dxg_process.v, device);
}

static int hvdxg_flush_device_host(uint32 device)
{
    struct hvdxg_command_flushdevice flush;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (device == 0)
        return 0;
    memset(&flush, 0, sizeof(flush));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&flush.hdr,
                                    HV_DXGK_VMBCOMMAND_FLUSHDEVICE,
                                    hvdxg.dxg_process);
    flush.device.v = device;
    flush.reason = HV_DXG_FLUSHSCHEDULER_DEVICE_TERMINATE;
    ret = hvdxg_send_sync_vgpu(&flush, sizeof(flush), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0)
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.flushdevice_last_len = actual_len;
    hvdxg.flushdevice_last_ret = ret;
    hvdxg.flushdevice_last_status = status.v;
    hvdxg.flushdevice_last_device = device;
    hvdxg.flushdevice_last_reason = HV_DXG_FLUSHSCHEDULER_DEVICE_TERMINATE;
    return ret;
}

static void hvdxg_note_destroyallocation(uint32 device, uint32 resource,
                                         uint32 allocation, uint32 process,
                                         uint32 context, uint32 count,
                                         uint32 actual_len, int ret,
                                         struct hvdxg_ntstatus status)
{
    uint32 match = 0;
    uint32 pending = 0;
    uint32 before_nt = 0;

    hvdxg.destroyalloc_last_device = device;
    hvdxg.destroyalloc_last_resource = resource;
    hvdxg.destroyalloc_last_allocation = allocation;
    hvdxg.destroyalloc_last_process = process;
    hvdxg.destroyalloc_last_context = context;
    hvdxg.destroyalloc_last_count = count;
    hvdxg.destroyalloc_last_len = actual_len;
    hvdxg.destroyalloc_last_ret = ret;
    hvdxg.destroyalloc_last_status = status.v;

    if (resource != 0 &&
        resource == hvdxg.d3d12_shared_alloc_resource_out)
        match |= 1U;
    if (allocation != 0 &&
        allocation == hvdxg.d3d12_shared_alloc_allocation)
        match |= 2U;
    if (match != 0) {
        uint64 seq = ++hvdxg.d3d12_shared_event_seq;

        hvdxg.destroyalloc_d3d12_last_seq = seq;
        hvdxg.destroyalloc_d3d12_match_count++;
        if (context < 32)
            hvdxg.destroyalloc_d3d12_context_mask |= 1U << context;
        if (hvdxg.sharedresource_owner_nt == 0 &&
            hvdxg.sharedresource_owner_sealed == 0) {
            pending = 1;
            hvdxg.destroyalloc_d3d12_pending_match_count++;
        }
        before_nt = hvdxg.d3d12_shared_first_nt_seq == 0 ||
                    seq < hvdxg.d3d12_shared_first_nt_seq;
        hvdxg.destroyalloc_d3d12_last_before_nt = before_nt;
        if (hvdxg.destroyalloc_d3d12_first_seq == 0) {
            hvdxg.destroyalloc_d3d12_first_seq = seq;
            hvdxg.destroyalloc_d3d12_first_context = context;
            hvdxg.destroyalloc_d3d12_first_match = match;
            hvdxg.destroyalloc_d3d12_first_pending = pending;
            hvdxg.destroyalloc_d3d12_first_before_nt = before_nt;
        }
    }
    hvdxg.destroyalloc_d3d12_last_match = match | (pending ? 4U : 0U);
}

static int hvdxg_destroy_allocation_host_process(
    uint32 process, uint32 device, uint32 resource,
    uint32 allocation, uint32 context)
{
    uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                      sizeof(struct hvdxg_d3dkmthandle)];
    struct hvdxg_command_destroyallocation *destroy =
        (struct hvdxg_command_destroyallocation *)command_buf;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    uint32 command_len;
    int ret;

    if (process == 0 || device == 0 || (resource == 0 && allocation == 0))
        return 0;
    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    hvdxg_command_vgpu_init_process(&destroy->hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                    process_handle);
    destroy->device.v = device;
    destroy->resource.v = resource;
    destroy->flags.assume_not_in_use = 1;
    if (resource == 0 && allocation != 0) {
        destroy->alloc_count = 1;
        destroy->allocations[0].v = allocation;
    }
    command_len = sizeof(*destroy) +
                  destroy->alloc_count * sizeof(destroy->allocations[0]);
    ret = hvdxg_send_sync_vgpu(destroy, command_len, &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg_note_destroyallocation(device, resource, allocation,
                                 destroy->hdr.process.v, context,
                                 destroy->alloc_count, actual_len, ret,
                                 status);
    return ret;
}

static int hvdxg_destroy_allocation_host(uint32 device, uint32 resource,
                                         uint32 allocation, uint32 context)
{
    return hvdxg_destroy_allocation_host_process(
        hvdxg.dxg_process.v, device, resource, allocation, context);
}

static int hvdxg_destroy_device_owned_objects(struct hvdxg_open_state *owner,
                                              uint32 device)
{
    int ret = 0;

    if (owner == NULL || device == 0)
        return 0;

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->sync_object_count; i++) {
            if (owner->sync_objects[i].device != device)
                continue;
            /*
             * WSL dxgdevice_destroy() calls dxgsyncobject_destroy() here,
             * which drops the handle and event mappings locally without a
             * DESTROYSYNCOBJECT VM-bus packet.
             */
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_SYNC,
                                   owner->sync_objects[i].sync);
            hvdxg_untrack_sync(owner, owner->sync_objects[i].sync);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        struct hvdxg_tracked_allocation a;
        int found = 0;

        for (uint32 i = 0; i < owner->allocation_count; i++) {
            a = owner->allocations[i];
            if (a.device != device || a.resource != 0 || a.allocation == 0)
                continue;
            hvdxg_untrack_allocation(owner, device, 0, a.allocation);
            (void)hvdxg_unmap_tracked_allocation(&a);
            hvdxg_unpin_tracked_allocation(&a);
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_allocation_host(
                    device, 0, a.allocation,
                    HV_DXG_DESTROY_ALLOC_CTX_DEVICE_STANDALONE),
                HV_DXG_CLEANUP_ALLOCATION, a.allocation);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        uint32 resource = 0;
        int found = 0;

        for (uint32 i = 0; i < owner->resource_count; i++) {
            resource = owner->resources[i].resource;
            if (owner->resources[i].device == device && resource != 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            break;

        for (;;) {
            int removed = 0;

            for (uint32 i = 0; i < owner->allocation_count; i++) {
                struct hvdxg_tracked_allocation a = owner->allocations[i];

                if (a.device != device || a.resource != resource)
                    continue;
                hvdxg_untrack_allocation(owner, device, resource,
                                         a.allocation);
                (void)hvdxg_unmap_tracked_allocation(&a);
                hvdxg_unpin_tracked_allocation(&a);
                hvdxg_cleanup_note_ret(&ret, 0,
                                       HV_DXG_CLEANUP_ALLOCATION,
                                       a.allocation);
                removed = 1;
                break;
            }
            if (!removed)
                break;
        }
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_allocation_host(
                device, resource, 0,
                HV_DXG_DESTROY_ALLOC_CTX_DEVICE_RESOURCE),
            HV_DXG_CLEANUP_RESOURCE, resource);
        hvdxg_untrack_resource(owner, device, resource);
    }

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->context_count; i++) {
            uint32 context = owner->contexts[i];
            uint32 sync = 0;

            if (hvdxg_owner_object_device(owner, HV_DXG_OBJECT_CONTEXT,
                                          context) != device)
                continue;
            /*
             * WSL dxgdevice_destroy() releases context handles locally; it
             * does not send DESTROYCONTEXT during process/device teardown.
             */
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_CONTEXT,
                                   context);
            hvdxg_untrack_object(owner, HV_DXG_OBJECT_CONTEXT, context);
            hvdxg_untrack_u32(owner->contexts, &owner->context_count,
                              context);
            for (;;) {
                int removed_hwqueue = 0;

                for (uint32 j = 0; j < owner->hwqueue_count; j++) {
                    uint32 queue;

                    if (owner->hwqueues[j].device != device ||
                        owner->hwqueues[j].context != context)
                        continue;
                    queue = owner->hwqueues[j].queue;
                    sync = hvdxg_untrack_hwqueue(owner, queue);
                    hvdxg_cleanup_note_ret(
                        &ret, 0, HV_DXG_CLEANUP_HWQUEUE, queue);
                    if (sync != 0)
                        hvdxg_untrack_sync(owner, sync);
                    removed_hwqueue = 1;
                    break;
                }
                if (!removed_hwqueue)
                    break;
            }
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        int found = 0;

        /*
         * If a context was never tracked successfully, still drop orphaned
         * HW-queue handles locally before paging queues, matching WSL's
         * "children before final device/process" cleanup intent.
         */
        for (uint32 i = 0; i < owner->hwqueue_count; i++) {
            uint32 queue = owner->hwqueues[i].queue;
            uint32 sync;

            if (owner->hwqueues[i].device != device)
                continue;
            sync = hvdxg_untrack_hwqueue(owner, queue);
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_HWQUEUE,
                                   queue);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->paging_queue_count; i++) {
            uint32 sync;
            uint32 queue = owner->paging_queues[i].queue;

            if (owner->paging_queues[i].device != device)
                continue;
            sync = hvdxg_untrack_pagingqueue(owner,
                                             queue);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
            /*
             * Paging queue and monitored-fence handles are dropped locally
             * during WSL device teardown.
             */
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_PAGINGQUEUE,
                                   queue);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    for (;;) {
        int found = 0;

        for (uint32 i = 0; i < owner->sync_object_count; i++) {
            if (owner->sync_objects[i].device != device)
                continue;
            hvdxg_untrack_sync(owner, owner->sync_objects[i].sync);
            found = 1;
            break;
        }
        if (!found)
            break;
    }

    return ret;
}

static int hvdxg_destroy_process_adapter_devices(
    struct hvdxg_open_state *owner, uint32 host_adapter)
{
    int ret = 0;

    if (owner == NULL || owner->process_state == NULL ||
        owner->process_state->adapters == NULL || host_adapter == 0)
        return 0;

    for (uint32 i = 0; i < owner->process_state->adapter_count; i++) {
        struct hvdxg_process_adapter *adapter =
            &owner->process_state->adapters[i];

        if (adapter->host_adapter_handle != host_adapter ||
            adapter->refs != 0)
            continue;
        while (adapter->device_count > 0) {
            uint32 device = adapter->devices[--adapter->device_count];

            adapter->devices[adapter->device_count] = 0;
            if (device == 0)
                continue;
            hvdxg_untrack_u32(owner->devices, &owner->device_count,
                              device);
            if (hvdxg_untrack_object(owner, HV_DXG_OBJECT_DEVICE,
                                     device)) {
                int flush_ret = hvdxg_flush_device_host(device);

                if (ret == 0 && flush_ret != 0) {
                    ret = flush_ret;
                    hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_DEVICE;
                    hvdxg.cleanup_failed_handle = device;
                }
                hvdxg_cleanup_note_ret(
                    &ret, hvdxg_destroy_device_owned_objects(owner, device),
                    HV_DXG_CLEANUP_DEVICE, device);
                hvdxg_cleanup_note_ret(
                    &ret, hvdxg_destroy_device_host(device),
                    HV_DXG_CLEANUP_DEVICE, device);
            }
        }
    }
    return ret;
}

static int hvdxg_destroy_createallocation_result(
    uint32 process, uint32 device, uint32 resource,
    const struct hvdxg_command_createallocation_return *result,
    uint32 alloc_count)
{
    uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                      HV_DXG_ALLOCATION_MAX *
                          sizeof(struct hvdxg_d3dkmthandle)];
    struct hvdxg_command_destroyallocation *destroy =
        (struct hvdxg_command_destroyallocation *)command_buf;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    uint32 command_len;
    int ret;

    if (process == 0 || device == 0 || result == NULL ||
        (resource == 0 && alloc_count == 0) ||
        alloc_count > HV_DXG_ALLOCATION_MAX)
        return 0;
    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    hvdxg_command_vgpu_init_process(&destroy->hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                    process_handle);
    destroy->device.v = device;
    destroy->resource.v = resource;
    destroy->alloc_count = alloc_count;
    destroy->flags.assume_not_in_use = 1;
    for (uint32 i = 0; i < alloc_count; i++)
        destroy->allocations[i] = result->allocation_info[i].allocation;
    command_len = sizeof(*destroy) +
                  alloc_count * sizeof(destroy->allocations[0]);
    ret = hvdxg_send_sync_vgpu(destroy, command_len, &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg_note_destroyallocation(
        device, resource,
        alloc_count != 0 ? result->allocation_info[0].allocation.v : 0,
        destroy->hdr.process.v, HV_DXG_DESTROY_ALLOC_CTX_CREATE_UNWIND,
        alloc_count, actual_len, ret, status);
    hvdxg.createalloc_unwind_attempts++;
    hvdxg.createalloc_unwind_last_ret = ret;
    if (ret == 0)
        hvdxg.createalloc_unwind_successes++;
    return ret;
}

static int hvdxg_destroy_context_host_process(uint32 process, uint32 context)
{
    struct hvdxg_command_destroycontext destroy;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    int ret;

    if (context == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    if (process_handle.v == 0)
        process_handle = hvdxg.dxg_process;
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYCONTEXT,
                                    process_handle);
    destroy.context.v = context;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (actual_len >= sizeof(status))
        hvdxg.destroycontext_last_status = status.v;
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroycontext_last_len = actual_len;
    hvdxg.destroycontext_last_ret = ret;
    return ret;
}

static int hvdxg_destroy_pagingqueue_host_process(uint32 process,
                                                  uint32 paging_queue)
{
    struct hvdxg_command_destroypagingqueue destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (paging_queue == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE,
                                    (struct hvdxg_d3dkmthandle){ .v = process });
    destroy.paging_queue.v = paging_queue;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    return ret;
}

static int hvdxg_destroy_hwqueue_host_process(uint32 process, uint32 hwqueue)
{
    struct hvdxg_command_destroyhwqueue destroy;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle process_handle;
    uint32 actual_len = 0;
    int ret;

    if (hwqueue == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    process_handle.v = process;
    if (process_handle.v == 0)
        process_handle = hvdxg.dxg_process;
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE,
                                    process_handle);
    destroy.hwqueue.v = hwqueue;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len < sizeof(status))
        ret = -EOVERFLOW;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroyhwqueue_last_len = actual_len;
    hvdxg.destroyhwqueue_last_ret = ret;
    return ret;
}

static void hvdxg_wait_gpuva_fence(const struct hvdxg_tracked_gpuva *gpuva)
{
    if (gpuva == NULL || gpuva->fence_value == 0 ||
        gpuva->fence_cpu_pa == 0)
        return;
    /*
     * The paging fence storage lives in Hyper-V I/O space. It is mapped into
     * the user process, but not into the kernel direct map on all hosts.
     */
    sleep_ms(20);
}

static void hvdxg_cleanup_note_ret(int *cleanup_ret, int op_ret,
                                   uint32 op, uint32 handle)
{
    hvdxg_cleanup_mark_wsl_order(op);
    hvdxg.cleanup_last_op = op;
    hvdxg.cleanup_last_handle = handle;
    if (*cleanup_ret == 0 && op_ret != 0) {
        *cleanup_ret = op_ret;
        hvdxg.cleanup_failed_op = op;
        hvdxg.cleanup_failed_handle = handle;
    }
}

static int hvdxg_cleanup_order_before(uint32 a, uint32 b)
{
    return a == 0 || b == 0 || a <= b;
}

static void hvdxg_cleanup_reset_wsl_order(void)
{
    hvdxg.cleanup_wsl_order_seq = 0;
    hvdxg.cleanup_wsl_order_sync = 0;
    hvdxg.cleanup_wsl_order_allocation = 0;
    hvdxg.cleanup_wsl_order_resource = 0;
    hvdxg.cleanup_wsl_order_context = 0;
    hvdxg.cleanup_wsl_order_hwqueue = 0;
    hvdxg.cleanup_wsl_order_pagingqueue = 0;
    hvdxg.cleanup_wsl_order_gpuva = 0;
    hvdxg.cleanup_wsl_order_device = 0;
    hvdxg.cleanup_wsl_order_process = 0;
    hvdxg.cleanup_wsl_order_valid = 0;
}

static void hvdxg_cleanup_mark_wsl_order(uint32 op)
{
    uint32 order;
    uint32 *slot = NULL;

    switch (op) {
    case HV_DXG_CLEANUP_SYNC:
        slot = &hvdxg.cleanup_wsl_order_sync;
        break;
    case HV_DXG_CLEANUP_ALLOCATION:
        slot = &hvdxg.cleanup_wsl_order_allocation;
        break;
    case HV_DXG_CLEANUP_RESOURCE:
        slot = &hvdxg.cleanup_wsl_order_resource;
        break;
    case HV_DXG_CLEANUP_CONTEXT:
        slot = &hvdxg.cleanup_wsl_order_context;
        break;
    case HV_DXG_CLEANUP_HWQUEUE:
        slot = &hvdxg.cleanup_wsl_order_hwqueue;
        break;
    case HV_DXG_CLEANUP_PAGINGQUEUE:
        slot = &hvdxg.cleanup_wsl_order_pagingqueue;
        break;
    case HV_DXG_CLEANUP_GPUVA:
        slot = &hvdxg.cleanup_wsl_order_gpuva;
        break;
    case HV_DXG_CLEANUP_DEVICE:
        slot = &hvdxg.cleanup_wsl_order_device;
        break;
    case HV_DXG_CLEANUP_NONE:
        slot = &hvdxg.cleanup_wsl_order_process;
        break;
    default:
        return;
    }
    if (slot == NULL || *slot != 0)
        return;
    order = ++hvdxg.cleanup_wsl_order_seq;
    if (order == 0)
        order = ++hvdxg.cleanup_wsl_order_seq;
    *slot = order;
}

static void hvdxg_cleanup_finalize_wsl_order(void)
{
    int valid = 1;

    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_sync,
        hvdxg.cleanup_wsl_order_allocation);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_allocation,
        hvdxg.cleanup_wsl_order_resource);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_resource,
        hvdxg.cleanup_wsl_order_context);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_context,
        hvdxg.cleanup_wsl_order_hwqueue);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_hwqueue,
        hvdxg.cleanup_wsl_order_pagingqueue);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_pagingqueue,
        hvdxg.cleanup_wsl_order_device);
    valid = valid && hvdxg_cleanup_order_before(
        hvdxg.cleanup_wsl_order_device,
        hvdxg.cleanup_wsl_order_process);
    hvdxg.cleanup_wsl_order_valid = valid ? 1 : 0;
}

static struct hvdxg_process_state *hvdxg_process_get_current(void)
{
    struct hvdxg_process_state *candidate;
    struct hvdxg_process_state *process = NULL;
    uint64 tgid = current ? (uint64)thread_tgid(current) : 1;
    int empty = -1;

    candidate = kvmalloc(sizeof(*candidate));
    if (candidate == NULL)
        return NULL;
    memset(candidate, 0, sizeof(*candidate));
    candidate->tgid = tgid;
    /*
     * WSL keys dxgprocess reuse by TGID, but the CREATEPROCESS packet carries
     * the creator thread pid.  Mesa can open /dev/dxg from worker threads, and
     * hosts may reject a CREATEPROCESS whose process_id does not match that
     * Linux thread identity.
     */
    candidate->pid = current ? (uint64)current->pid : tgid;
    candidate->process_refs = 1;
    candidate->process_mem_refs = 1;

    mutex_lock(&hvdxg.process_lock);
    for (uint32 i = 0; i < HV_DXG_PROCESS_TABLE_MAX; i++) {
        struct hvdxg_process_state *entry = hvdxg.processes[i];

        if (entry == NULL) {
            if (empty < 0)
                empty = (int)i;
            continue;
        }
        if (entry->tgid == tgid && entry->process_refs != 0) {
            entry->process_refs++;
            entry->process_mem_refs++;
            entry->pid = current ? (uint64)current->pid : tgid;
            hvdxg.process_retained_handle = entry->host_process.v;
            hvdxg.process_retained_generation = entry->generation;
            hvdxg.process_retained_tgid = entry->tgid;
            hvdxg.process_retained_refs = entry->process_refs;
            hvdxg.process_object_refs_last = entry->process_refs;
            hvdxg.process_mem_refs_last = entry->process_mem_refs;
            hvdxg.process_reuses++;
            hvdxg.process_shared_reuses++;
            process = entry;
            break;
        }
    }
    if (process == NULL && empty >= 0) {
        uint32 retained_handle;
        uint32 retained_generation;
        uint64 retained_tgid;

        process = candidate;
        process->generation = ++hvdxg.process_generation;
        if (process->generation == 0)
            process->generation = ++hvdxg.process_generation;
        process->guest_process = (uint64)process;
        if (hvdxg.dxg_process_created && hvdxg.dxg_process.v != 0) {
            retained_handle = hvdxg.dxg_process.v;
            retained_tgid = hvdxg.dxg_process_tgid;
            retained_generation = hvdxg.dxg_process_generation;
        } else {
            retained_handle = hvdxg.process_retained_handle;
            retained_tgid = hvdxg.process_retained_tgid;
            retained_generation = hvdxg.process_retained_generation;
        }
        if (retained_handle != 0 &&
            (retained_tgid == 0 || retained_tgid != process->tgid ||
             retained_generation != process->generation)) {
            hvdxg.process_retained_reuse_avoided++;
            hvdxg.process_retained_avoided_tgid = process->tgid;
            hvdxg.process_retained_avoided_source_tgid =
                retained_tgid;
            hvdxg.process_retained_avoided_handle = retained_handle;
            hvdxg.process_retained_avoided_generation =
                process->generation;
            hvdxg.process_retained_avoided_source_generation =
                retained_generation;
        }
        hvdxg.process_isolated_last_handle = 0;
        hvdxg.processes[empty] = process;
        hvdxg.process_live++;
        hvdxg.process_creates++;
        hvdxg.process_isolated_last_tgid = process->tgid;
        hvdxg.process_isolated_last_generation = process->generation;
        hvdxg.process_isolated_source_generation = 0;
        hvdxg.process_isolated_copied_objects = 0;
        hvdxg.process_isolated_source_objects = 0;
        if (hvdxg.process_live > hvdxg.process_live_max)
            hvdxg.process_live_max = hvdxg.process_live;
        hvdxg.process_object_refs_last = process->process_refs;
        hvdxg.process_mem_refs_last = process->process_mem_refs;
        candidate = NULL;
    } else if (process == NULL) {
        hvdxg.process_table_full++;
    }
    mutex_unlock(&hvdxg.process_lock);

    if (candidate != NULL)
        kvfree(candidate);
    if (process != NULL) {
        hvdxg.process_identity_pid_present = process->pid != 0;
        hvdxg.process_identity_tgid_present = process->tgid != 0;
        hvdxg.process_identity_vpid_present = 0;
        hvdxg.process_identity_nspid_present = 0;
    }
    return process;
}

static void hvdxg_process_memory_put(struct hvdxg_process_state *process)
{
    int free_process = 0;

    if (process == NULL)
        return;

    mutex_lock(&hvdxg.process_lock);
    if (process->process_mem_refs > 0)
        process->process_mem_refs--;
    hvdxg.process_mem_releases++;
    hvdxg.process_object_refs_last = process->process_refs;
    hvdxg.process_mem_refs_last = process->process_mem_refs;
    if (process->process_mem_refs == 0) {
        free_process = 1;
        hvdxg.process_mem_frees++;
    }
    mutex_unlock(&hvdxg.process_lock);

    if (free_process) {
        if (process->objects != NULL)
            kvfree(process->objects);
        if (process->adapters != NULL) {
            for (uint32 i = 0; i < process->adapter_count; i++) {
                if (process->adapters[i].devices != NULL)
                    kvfree(process->adapters[i].devices);
            }
            kvfree(process->adapters);
        }
        if (process->local_adapters != NULL)
            kvfree(process->local_adapters);
        kvfree(process);
    }
}

static int hvdxg_process_put(struct hvdxg_process_state *process)
{
    int remove_process = 0;
    int destroy_host = 0;
    int ret = 0;
    struct hvdxg_d3dkmthandle host_process;

    if (process == NULL)
        return 0;
    memset(&host_process, 0, sizeof(host_process));

    mutex_lock(&hvdxg.process_lock);
    if (process->process_refs > 0)
        process->process_refs--;
    hvdxg.process_object_refs_last = process->process_refs;
    hvdxg.process_mem_refs_last = process->process_mem_refs;
    if (process->process_refs == 0) {
        hvdxg.process_releases++;
        if (process->host_process_created && process->host_process.v != 0) {
            hvdxg.process_destroy_active_total = 0;
            hvdxg.process_destroy_active_device = 0;
            hvdxg.process_destroy_active_context = 0;
            hvdxg.process_destroy_active_hwqueue = 0;
            hvdxg.process_destroy_active_pagingqueue = 0;
            hvdxg.process_destroy_active_sync = 0;
            hvdxg.process_destroy_active_allocation = 0;
            hvdxg.process_destroy_active_resource = 0;
            hvdxg.process_destroy_active_gpuva = 0;
            for (uint32 i = 0; i < process->object_count; i++) {
                if (process->objects == NULL ||
                    process->objects[i].type == HV_DXG_OBJECT_NONE ||
                    process->objects[i].destroyed != 0)
                    continue;
                hvdxg.process_destroy_active_total++;
                switch (process->objects[i].type) {
                case HV_DXG_OBJECT_ADAPTER:
                    break;
                case HV_DXG_OBJECT_DEVICE:
                    hvdxg.process_destroy_active_device++;
                    break;
                case HV_DXG_OBJECT_CONTEXT:
                    hvdxg.process_destroy_active_context++;
                    break;
                case HV_DXG_OBJECT_HWQUEUE:
                    hvdxg.process_destroy_active_hwqueue++;
                    break;
                case HV_DXG_OBJECT_PAGINGQUEUE:
                    hvdxg.process_destroy_active_pagingqueue++;
                    break;
                case HV_DXG_OBJECT_SYNC:
                    hvdxg.process_destroy_active_sync++;
                    break;
                case HV_DXG_OBJECT_ALLOCATION:
                    hvdxg.process_destroy_active_allocation++;
                    break;
                case HV_DXG_OBJECT_RESOURCE:
                    hvdxg.process_destroy_active_resource++;
                    break;
                case HV_DXG_OBJECT_GPUVA:
                    hvdxg.process_destroy_active_gpuva++;
                    break;
                default:
                    break;
                }
            }
            host_process = process->host_process;
            destroy_host = 1;
            remove_process = 1;
            process->host_process.v = 0;
            process->host_process_created = 0;
            if (hvdxg.dxg_process.v == host_process.v) {
                hvdxg.dxg_process.v = 0;
                hvdxg.dxg_process_created = 0;
                hvdxg.dxg_process_guest = 0;
                hvdxg.dxg_process_pid = 0;
                hvdxg.dxg_process_tgid = 0;
                hvdxg.d3dkmt_ready = 0;
            }
        } else {
            remove_process = 1;
        }
    }
    mutex_unlock(&hvdxg.process_lock);

    if (destroy_host)
        ret = hvdxg_destroy_process_host(host_process);

    if (remove_process) {
        mutex_lock(&hvdxg.process_lock);
        for (uint32 i = 0; i < HV_DXG_PROCESS_TABLE_MAX; i++) {
            if (hvdxg.processes[i] == process) {
                hvdxg.processes[i] = NULL;
                break;
            }
        }
        if (hvdxg.process_live > 0)
            hvdxg.process_live--;
        mutex_unlock(&hvdxg.process_lock);
    }
    hvdxg_process_memory_put(process);
    return ret;
}

static uint32 hvdxg_process_refs(struct hvdxg_process_state *process)
{
    uint32 refs = 0;

    if (process == NULL)
        return 0;
    mutex_lock(&hvdxg.process_lock);
    refs = process->process_refs;
    mutex_unlock(&hvdxg.process_lock);
    return refs;
}

#define HV_DXG_BIND_SOURCE_ENUMADAPTERS2       1U
#define HV_DXG_BIND_SOURCE_ENUMADAPTERS3       2U
#define HV_DXG_BIND_SOURCE_OPENADAPTERFROMLUID 3U

static const char *hvdxg_early_bind_source_name(uint32 source)
{
    switch (source) {
    case HV_DXG_BIND_SOURCE_ENUMADAPTERS2:
        return "enumadapters2";
    case HV_DXG_BIND_SOURCE_ENUMADAPTERS3:
        return "enumadapters3";
    case HV_DXG_BIND_SOURCE_OPENADAPTERFROMLUID:
        return "openadapterfromluid";
    default:
        return "none";
    }
}

static void hvdxg_note_open_createprocess(struct hvdxg_open_state *owner,
                                          int ret)
{
    if (owner == NULL)
        return;
    hvdxg.open_createprocess_attempts++;
    hvdxg.open_createprocess_last_ret = ret;
    hvdxg.open_createprocess_last_guest = owner->dxg_process_guest;
    hvdxg.open_createprocess_last_pid = owner->dxg_process_pid;
    hvdxg.open_createprocess_last_tgid =
        owner->process_state != NULL ? owner->process_state->tgid :
        owner->dxg_process_pid;
    hvdxg.open_createprocess_last_handle = owner->dxg_process.v;
    hvdxg.open_createprocess_last_created = owner->dxg_process_created;
    hvdxg.open_createprocess_last_generation =
        owner->process_state != NULL ? owner->process_state->generation : 0;
    hvdxg.open_createprocess_last_refs =
        owner->process_state != NULL ?
            owner->process_state->process_refs : 0;
    if (ret == 0)
        hvdxg.open_createprocess_successes++;
    else
        hvdxg.open_createprocess_failures++;
}

static int hvdxg_bind_open_process(struct hvdxg_open_state *owner)
{
    struct hvdxg_process_state *process;
    int ret;
    int create_attempt;

    if (owner == NULL)
        return 0;
    process = owner->process_state;
    if (process != NULL) {
        create_attempt = !process->host_process_created ||
                         process->host_process.v == 0;
        hvdxg.dxg_process = process->host_process;
        hvdxg.dxg_process_created = process->host_process_created;
        hvdxg.dxg_process_guest = process->guest_process;
        hvdxg.dxg_process_pid = process->pid;
        hvdxg.dxg_process_tgid = process->tgid;
        hvdxg.dxg_process_generation = process->generation;
        if (create_attempt)
            hvdxg.d3dkmt_ready = 0;
        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0) {
            if (create_attempt) {
                process->host_process.v = 0;
                process->host_process_created = 0;
                owner->dxg_process.v = 0;
                owner->dxg_process_created = 0;
                hvdxg.dxg_process.v = 0;
                hvdxg.dxg_process_created = 0;
                hvdxg.d3dkmt_ready = 0;
                hvdxg_note_open_createprocess(owner, ret);
            }
            return ret;
        }
        process->host_process = hvdxg.dxg_process;
        process->host_process_created = hvdxg.dxg_process_created;
        owner->dxg_process = process->host_process;
        owner->dxg_process_guest = process->guest_process;
        owner->dxg_process_pid = process->pid;
        owner->dxg_process_created = process->host_process_created;
        hvdxg.dxg_process_tgid = process->tgid;
        hvdxg.dxg_process_generation = process->generation;
        if (create_attempt)
            hvdxg_note_open_createprocess(owner, ret);
        return 0;
    }
    if (!owner->dxg_process_created || owner->dxg_process.v == 0) {
        create_attempt = 1;
        hvdxg.dxg_process.v = 0;
        hvdxg.dxg_process_created = 0;
        hvdxg.dxg_process_guest = owner->dxg_process_guest;
        hvdxg.dxg_process_pid = owner->dxg_process_pid;
        hvdxg.dxg_process_tgid = owner->dxg_process_pid;
        hvdxg.dxg_process_generation = 0;
        hvdxg.d3dkmt_ready = 0;
        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0) {
            owner->dxg_process.v = 0;
            owner->dxg_process_created = 0;
            hvdxg.dxg_process.v = 0;
            hvdxg.dxg_process_created = 0;
            hvdxg.d3dkmt_ready = 0;
            hvdxg_note_open_createprocess(owner, ret);
            return ret;
        }
        owner->dxg_process = hvdxg.dxg_process;
        owner->dxg_process_guest = hvdxg.dxg_process_guest;
        owner->dxg_process_pid = hvdxg.dxg_process_pid;
        owner->dxg_process_created = hvdxg.dxg_process_created;
        if (create_attempt)
            hvdxg_note_open_createprocess(owner, ret);
        return 0;
    }
    hvdxg.dxg_process = owner->dxg_process;
    hvdxg.dxg_process_guest = owner->dxg_process_guest;
    hvdxg.dxg_process_pid = owner->dxg_process_pid;
    hvdxg.dxg_process_tgid = owner->dxg_process_pid;
    hvdxg.dxg_process_generation = 0;
    hvdxg.dxg_process_created = owner->dxg_process_created;
    if (!hvdxg.dxg_process_created || hvdxg.dxg_process.v == 0)
        hvdxg.d3dkmt_ready = 0;
    ret = hvdxg_d3dkmt_ensure();
    if (ret != 0) {
        if (!hvdxg.dxg_process_created || hvdxg.dxg_process.v == 0)
            hvdxg.d3dkmt_ready = 0;
        return ret;
    }
    return 0;
}

static int hvdxg_should_defer_open_create(
    struct hvdxg_process_state *process)
{
    if (process == NULL)
        return 0;
    if (process->host_process_created && process->host_process.v != 0)
        return 0;
    return hvdxg.process_retained_handle != 0;
}

static int hvdxg_bind_open_process_early(struct hvdxg_open_state *owner,
                                         uint32 source, uint32 cmd)
{
    int ret;

    hvdxg.early_bind_attempts++;
    hvdxg.early_bind_last_source = source;
    hvdxg.early_bind_last_cmd = cmd;
    hvdxg.early_bind_last_handle = 0;
    hvdxg.early_bind_last_created = 0;
    hvdxg.early_bind_last_generation = 0;
    hvdxg.early_bind_last_refs = 0;

    if (owner != NULL) {
        mutex_lock(&hvdxg.process_lock);
        ret = hvdxg_bind_open_process(owner);
        hvdxg.early_bind_last_handle = owner->dxg_process.v;
        hvdxg.early_bind_last_created = owner->dxg_process_created;
        if (owner->process_state != NULL) {
            hvdxg.early_bind_last_generation =
                owner->process_state->generation;
            hvdxg.early_bind_last_refs =
                owner->process_state->process_refs;
        }
        mutex_unlock(&hvdxg.process_lock);
    } else {
        ret = hvdxg_d3dkmt_ensure();
        hvdxg.early_bind_last_handle = hvdxg.dxg_process.v;
        hvdxg.early_bind_last_created = hvdxg.dxg_process_created;
        hvdxg.early_bind_last_generation = hvdxg.dxg_process_generation;
    }

    hvdxg.early_bind_last_ret = ret;
    if (ret == 0)
        hvdxg.early_bind_successes++;
    else
        hvdxg.early_bind_failures++;
    return ret;
}

static void hvdxg_cleanup_open_state(struct hvdxg_open_state *owner)
{
    int had_tracked;
    int ret = 0;

    if (owner == NULL)
        return;
    if (owner->dxg_process_created && owner->dxg_process.v != 0) {
        hvdxg.dxg_process = owner->dxg_process;
        hvdxg.dxg_process_created = 1;
        hvdxg.dxg_process_guest = owner->dxg_process_guest;
        hvdxg.dxg_process_pid = owner->dxg_process_pid;
        if (owner->process_state != NULL) {
            hvdxg.dxg_process_tgid = owner->process_state->tgid;
            hvdxg.dxg_process_generation = owner->process_state->generation;
        }
        hvdxg.d3dkmt_ready = 1;
    } else if (owner->process_state == NULL && !hvdxg.d3dkmt_ready) {
        return;
    }
    had_tracked = owner->device_count != 0 || owner->allocation_count != 0 ||
                  owner->gpuva_count != 0 || owner->sync_object_count != 0 ||
                  owner->paging_queue_count != 0 ||
                  owner->context_count != 0 || owner->hwqueue_count != 0 ||
                  hvdxg_owner_has_active_process_objects(owner) ||
                  (owner->process_state != NULL &&
                   owner->process_state->local_adapter_count != 0);
    hvdxg.cleanup_last_op = HV_DXG_CLEANUP_NONE;
    hvdxg.cleanup_last_handle = 0;
    hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_NONE;
    hvdxg.cleanup_failed_handle = 0;
    hvdxg.cleanup_had_tracked = had_tracked ? 1 : 0;
    hvdxg_cleanup_reset_wsl_order();

    /*
     * WSL binds all /dev/dxg opens in one TGID to one dxgprocess.  A non-final
     * file close only drops the process reference so a secondary fd can keep
     * using handles created before another fd closes.  The final close tears
     * down the local object graph and sends DESTROYPROCESS after children are
     * destroyed, matching WSL's dxgprocess release ordering.
     */
    if (owner->process_state != NULL &&
        hvdxg_process_refs(owner->process_state) > 1) {
        uint32 handle = owner->dxg_process.v;

        hvdxg_cleanup_note_ret(
            &ret, hvdxg_process_put(owner->process_state),
            HV_DXG_CLEANUP_NONE, handle);
        owner->process_state = NULL;
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
        hvdxg.cleanup_attempts++;
        hvdxg.cleanup_last_ret = ret;
        hvdxg_cleanup_finalize_wsl_order();
        if (ret == 0)
            hvdxg.cleanup_successes++;
        return;
    }

    if (owner->gpuvas == NULL)
        owner->gpuva_count = 0;
    while (owner->gpuva_count > 0) {
        struct hvdxg_tracked_gpuva g =
            owner->gpuvas[--owner->gpuva_count];
        int gpuva_ret;
        int active;

        active = hvdxg_untrack_object(owner, HV_DXG_OBJECT_GPUVA, g.base);
        hvdxg_wait_gpuva_fence(&g);
        /*
         * WSL does not send FREEGPUVIRTUALADDRESS for live reservations from
         * process teardown.  Only explicit D3DKMT free ioctls reach the host.
         */
        gpuva_ret = active ? 0 : 0;
        hvdxg_cleanup_note_ret(&ret, gpuva_ret,
                               HV_DXG_CLEANUP_GPUVA, (uint32)g.base);
    }
    if (owner->hwqueues == NULL)
        owner->hwqueue_count = 0;
    if (owner->sync_objects == NULL)
        owner->sync_object_count = 0;
    if (owner->contexts == NULL)
        owner->context_count = 0;
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    if (owner->resources == NULL)
        owner->resource_count = 0;
    if (owner->paging_queues == NULL)
        owner->paging_queue_count = 0;
    if (owner->devices == NULL)
        owner->device_count = 0;
    while (owner->device_count > 0) {
        uint32 handle = owner->devices[--owner->device_count];

        if (hvdxg_untrack_object(owner, HV_DXG_OBJECT_DEVICE, handle)) {
            int flush_ret = hvdxg_flush_device_host(handle);

            if (ret == 0 && flush_ret != 0) {
                ret = flush_ret;
                hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_DEVICE;
                hvdxg.cleanup_failed_handle = handle;
            }
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_device_owned_objects(owner, handle),
                HV_DXG_CLEANUP_DEVICE, handle);
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_device_host(handle),
                HV_DXG_CLEANUP_DEVICE, handle);
        }
    }
    if (owner->hwqueues == NULL)
        owner->hwqueue_count = 0;
    while (owner->hwqueue_count > 0) {
        uint32 handle = owner->hwqueues[owner->hwqueue_count - 1].queue;
        uint32 sync = hvdxg_untrack_hwqueue(owner, handle);

        if (sync != 0)
            hvdxg_untrack_sync(owner, sync);
        hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_HWQUEUE, handle);
    }
    if (owner->contexts == NULL)
        owner->context_count = 0;
    while (owner->context_count > 0) {
        uint32 handle = owner->contexts[--owner->context_count];
        if (hvdxg_untrack_object(owner, HV_DXG_OBJECT_CONTEXT, handle))
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_CONTEXT, handle);
    }
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    while (owner->allocation_count > 0) {
        struct hvdxg_tracked_allocation a =
            owner->allocations[--owner->allocation_count];
        int active = 0;
        if (a.allocation != 0)
            active = hvdxg_untrack_object(owner, HV_DXG_OBJECT_ALLOCATION,
                                          a.allocation);
        (void)hvdxg_unmap_tracked_allocation(&a);
        hvdxg_unpin_tracked_allocation(&a);
        if (active)
            hvdxg_cleanup_note_ret(
                &ret,
                hvdxg_destroy_allocation_host(a.device, a.resource,
                                             a.allocation,
                                             HV_DXG_DESTROY_ALLOC_CTX_FILE_CLEANUP),
                HV_DXG_CLEANUP_ALLOCATION,
                a.allocation != 0 ? a.allocation : a.resource);
    }
    if (owner->resources == NULL)
        owner->resource_count = 0;
    while (owner->resource_count > 0) {
        struct hvdxg_tracked_resource *r =
            &owner->resources[--owner->resource_count];

        hvdxg_untrack_object(owner, HV_DXG_OBJECT_RESOURCE, r->resource);
        hvdxg_shared_parent_remove_resource(r);
        hvdxg_free_tracked_resource(r);
    }
    if (owner->paging_queues == NULL)
        owner->paging_queue_count = 0;
    while (owner->paging_queue_count > 0) {
        uint32 handle =
            owner->paging_queues[owner->paging_queue_count - 1].queue;
        uint32 sync = hvdxg_untrack_pagingqueue(owner, handle);

        if (sync != 0)
            hvdxg_untrack_sync(owner, sync);
        hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_PAGINGQUEUE, handle);
    }
    if (owner->sync_objects == NULL)
        owner->sync_object_count = 0;
    while (owner->sync_object_count > 0) {
        uint32 handle =
            owner->sync_objects[owner->sync_object_count - 1].sync;
        uint32 monitor_fence =
            owner->sync_objects[owner->sync_object_count - 1].
                monitor_fence_handle;

        hvdxg_untrack_sync(owner, handle);
        if (monitor_fence) {
            hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_SYNC, handle);
            continue;
        }
        hvdxg_cleanup_note_ret(&ret, 0, HV_DXG_CLEANUP_SYNC, handle);
    }
    hvdxg_cleanup_process_objects(owner, &ret);
    if (owner->process_state != NULL) {
        uint32 handle = owner->dxg_process.v;

        hvdxg_cleanup_mark_wsl_order(HV_DXG_CLEANUP_NONE);
        hvdxg_cleanup_note_ret(
            &ret, hvdxg_process_put(owner->process_state),
            HV_DXG_CLEANUP_NONE, handle);
        owner->process_state = NULL;
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
    } else if (owner->dxg_process_created && owner->dxg_process.v != 0) {
        uint32 handle = owner->dxg_process.v;

        hvdxg_cleanup_mark_wsl_order(HV_DXG_CLEANUP_NONE);
        hvdxg_cleanup_note_ret(
            &ret, hvdxg_destroy_process_host(owner->dxg_process),
            HV_DXG_CLEANUP_NONE, handle);
        owner->dxg_process.v = 0;
        owner->dxg_process_created = 0;
    }
    hvdxg_cleanup_finalize_wsl_order();
    hvdxg.cleanup_attempts++;
    hvdxg.cleanup_last_ret = ret;
    if (ret == 0)
        hvdxg.cleanup_successes++;
    (void)had_tracked;
}

static void hvdxg_free_open_state(struct hvdxg_open_state *owner)
{
    if (owner == NULL)
        return;
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    while (owner->allocation_count > 0) {
        struct hvdxg_tracked_allocation *a =
            &owner->allocations[--owner->allocation_count];

        (void)hvdxg_unmap_tracked_allocation(a);
        hvdxg_unpin_tracked_allocation(a);
    }
    if (owner->resources == NULL)
        owner->resource_count = 0;
    while (owner->resource_count > 0) {
        struct hvdxg_tracked_resource *r =
            &owner->resources[--owner->resource_count];

        hvdxg_shared_parent_remove_resource(r);
        hvdxg_free_tracked_resource(r);
    }
    owner->gpuva_count = 0;
    owner->object_count = 0;
    owner->object_free_count = 0;
    owner->object_free_head = HV_DXG_HMGR_FREE_NONE;
    owner->object_free_tail = HV_DXG_HMGR_FREE_NONE;
    if (owner->read_status != NULL)
        kvfree(owner->read_status);
    if (owner->allocations != NULL)
        kvfree(owner->allocations);
    if (owner->resources != NULL)
        kvfree(owner->resources);
    if (owner->gpuvas != NULL)
        kvfree(owner->gpuvas);
    if (owner->devices != NULL)
        kvfree(owner->devices);
    if (owner->contexts != NULL)
        kvfree(owner->contexts);
    if (owner->objects != NULL)
        kvfree(owner->objects);
    if (owner->hwqueues != NULL)
        kvfree(owner->hwqueues);
    if (owner->paging_queues != NULL)
        kvfree(owner->paging_queues);
    if (owner->sync_objects != NULL)
        kvfree(owner->sync_objects);
    kvfree(owner);
}
