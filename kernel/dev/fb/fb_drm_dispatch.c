static int gpu_drm_gem_close(struct fb_gpu_render_owner *owner, uint64 arg)
{
    struct drm_gem_close_compat req;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;
    ret = fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
    if (ret == -ENOENT)
        ret = virtio_gpu_user_resource_destroy(owner->id, owner->tgid,
                                               req.handle);
    return ret;
}

static int gpu_drm_ioctl_handle(struct drm_core_file *drm_file,
                                void *driver_file, uint64 cmd, uint64 arg)
{
    struct fb_gpu_render_owner *owner =
        (struct fb_gpu_render_owner *)driver_file;

    (void)drm_file;
    if (owner == NULL)
        return -EBADF;
    switch (cmd) {
    case DRM_IOCTL_VERSION:
        return gpu_drm_version(arg);
    case DRM_IOCTL_GET_UNIQUE:
        return gpu_drm_get_unique(arg);
    case DRM_IOCTL_GET_MAGIC:
        return gpu_drm_get_magic(owner, arg);
    case DRM_IOCTL_GET_MAP:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_get_map(arg);
    case DRM_IOCTL_AUTH_MAGIC:
        return gpu_drm_auth_magic(owner, arg);
    case DRM_IOCTL_GET_CLIENT:
        return gpu_drm_get_client(owner, arg);
    case DRM_IOCTL_GET_STATS:
        return gpu_drm_get_stats(arg);
    case DRM_IOCTL_SET_VERSION:
        return gpu_drm_set_version(arg);
    case DRM_IOCTL_GET_CAP:
        return gpu_drm_get_cap(arg);
    case DRM_IOCTL_SET_CLIENT_CAP:
        return gpu_drm_set_client_cap(owner, arg);
    case DRM_IOCTL_SET_CLIENT_NAME:
        return gpu_drm_set_client_name(owner, arg);
    case DRM_IOCTL_ADD_MAP:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_add_rm_map(arg, 0);
    case DRM_IOCTL_RM_MAP:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_add_rm_map(arg, 1);
    case DRM_IOCTL_SET_SAREA_CTX:
    case DRM_IOCTL_GET_SAREA_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_sarea_ctx(arg);
    case DRM_IOCTL_ADD_BUFS:
    case DRM_IOCTL_MARK_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_buf_desc(arg);
    case DRM_IOCTL_INFO_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_info_bufs(arg);
    case DRM_IOCTL_MAP_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_map_bufs(arg);
    case DRM_IOCTL_FREE_BUFS:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_free_bufs(arg);
    case DRM_IOCTL_SET_MASTER:
        return gpu_drm_set_master(owner);
    case DRM_IOCTL_DROP_MASTER:
        return gpu_drm_drop_master(owner);
    case DRM_IOCTL_ADD_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_ctx_mutate(arg, 1);
    case DRM_IOCTL_RM_CTX:
    case DRM_IOCTL_MOD_CTX:
    case DRM_IOCTL_SWITCH_CTX:
    case DRM_IOCTL_NEW_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_ctx_mutate(arg, 0);
    case DRM_IOCTL_GET_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_get_ctx(arg);
    case DRM_IOCTL_RES_CTX:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_res_ctx(arg);
    case DRM_IOCTL_DMA:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_dma(arg);
    case DRM_IOCTL_LOCK:
    case DRM_IOCTL_UNLOCK:
    case DRM_IOCTL_FINISH:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_lock(arg);
    case DRM_IOCTL_GEM_CLOSE:
        return gpu_drm_gem_close(owner, arg);
    case DRM_IOCTL_GEM_FLINK: {
        struct drm_gem_flink_compat req;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = fb_gem_flink(req.handle, owner->id, owner->tgid, &req.name);
        if (ret != 0)
            return ret;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_GEM_OPEN: {
        struct drm_gem_open_compat req;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.name == 0)
            return -EINVAL;
        ret = fb_gem_open_name(owner->id, owner->tgid, req.name,
                               &req.handle, &req.size);
        if (ret != 0)
            return ret;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0) {
            (void)fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
            return -EFAULT;
        }
        return 0;
    }
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:
        return gpu_drm_prime_handle_to_fd(owner, arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:
        return gpu_drm_prime_fd_to_handle(owner, arg);
    case DRM_IOCTL_AGP_ACQUIRE:
    case DRM_IOCTL_AGP_RELEASE:
    case DRM_IOCTL_AGP_INFO:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return -EOPNOTSUPP;
    case DRM_IOCTL_AGP_ENABLE:
    case DRM_IOCTL_AGP_ALLOC:
    case DRM_IOCTL_AGP_FREE:
    case DRM_IOCTL_AGP_BIND:
    case DRM_IOCTL_AGP_UNBIND:
    case DRM_IOCTL_SG_ALLOC:
    case DRM_IOCTL_SG_FREE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_legacy_agp_struct(arg, cmd);
    case DRM_IOCTL_WAIT_VBLANK:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_wait_vblank(arg);
    case DRM_IOCTL_CRTC_GET_SEQUENCE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_crtc_get_sequence(arg);
    case DRM_IOCTL_CRTC_QUEUE_SEQUENCE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_crtc_queue_sequence(arg);
    case DRM_IOCTL_MODE_GETRESOURCES:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getresources(owner, arg);
    case DRM_IOCTL_MODE_GETCRTC:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getcrtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:
        return gpu_drm_mode_setcrtc(owner, arg);
    case DRM_IOCTL_MODE_CURSOR:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_cursor(arg, 0);
    case DRM_IOCTL_MODE_GETGAMMA:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_gamma(arg, 0);
    case DRM_IOCTL_MODE_SETGAMMA:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_gamma(arg, 1);
    case DRM_IOCTL_MODE_GETENCODER:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETPROPERTY:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getproperty(arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getblob(arg);
    case DRM_IOCTL_MODE_GETFB:
        return gpu_drm_mode_getfb(owner, arg);
    case DRM_IOCTL_MODE_ADDFB:
        return -EOPNOTSUPP;
    case DRM_IOCTL_MODE_GETPLANERESOURCES:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getplaneresources(arg);
    case DRM_IOCTL_MODE_GETPLANE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_getplane(arg);
    case DRM_IOCTL_MODE_SETPLANE:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_setplane(owner, arg);
    case DRM_IOCTL_MODE_ADDFB2:
        return gpu_drm_mode_addfb2(owner, arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return gpu_drm_mode_obj_getproperties(owner, arg);
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
        return gpu_drm_mode_obj_setproperty(owner, arg);
    case DRM_IOCTL_MODE_RMFB:
        return gpu_drm_mode_rmfb(owner, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:
        return gpu_drm_mode_page_flip(owner, arg);
    case DRM_IOCTL_MODE_DIRTYFB:
        return gpu_drm_mode_dirtyfb(owner, arg);
    case DRM_IOCTL_MODE_ATOMIC:
        return gpu_drm_mode_atomic(owner, arg);
    case DRM_IOCTL_MODE_CURSOR2:
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_cursor(arg, 1);
    case DRM_IOCTL_MODE_CREATEPROPBLOB: {
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_createblob(arg);
    }
    case DRM_IOCTL_MODE_DESTROYPROPBLOB: {
        if (!gpu_drm_is_primary_like(owner))
            return -EOPNOTSUPP;
        return gpu_drm_mode_destroyblob(arg);
    }
    case DRM_IOCTL_MODE_CREATE_LEASE:
    case DRM_IOCTL_MODE_LIST_LESSEES:
    case DRM_IOCTL_MODE_GET_LEASE:
    case DRM_IOCTL_MODE_REVOKE_LEASE:
        return gpu_drm_mode_lease_fail_closed(owner, cmd, arg);
    case DRM_IOCTL_SYNCOBJ_CREATE:
        return gpu_syncobj_create(owner, arg);
    case DRM_IOCTL_SYNCOBJ_DESTROY:
        return gpu_syncobj_destroy(owner, arg);
    case DRM_IOCTL_XV6_SYNCOBJ_HANDLE_TO_FD_LEGACY:
        return gpu_syncobj_handle_to_fd(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
        return gpu_syncobj_handle_to_fd(owner, arg, 0);
    case DRM_IOCTL_XV6_SYNCOBJ_FD_TO_HANDLE_LEGACY:
        return gpu_syncobj_fd_to_handle(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
        return gpu_syncobj_fd_to_handle(owner, arg, 0);
    case DRM_IOCTL_SYNCOBJ_WAIT:
        return gpu_syncobj_wait_common(owner, arg, 0);
    case DRM_IOCTL_SYNCOBJ_RESET:
        return gpu_syncobj_array_signal_reset(owner, arg, 0, 0);
    case DRM_IOCTL_SYNCOBJ_SIGNAL:
        return gpu_syncobj_array_signal_reset(owner, arg, 1, 0);
    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
        return gpu_syncobj_wait_common(owner, arg, 1);
    case DRM_IOCTL_SYNCOBJ_QUERY:
        return gpu_syncobj_query(owner, arg);
    case DRM_IOCTL_SYNCOBJ_TRANSFER:
        return gpu_syncobj_transfer(owner, arg);
    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
        return gpu_syncobj_array_signal_reset(owner, arg, 1, 1);
    case DRM_IOCTL_MODE_GETFB2:
        return gpu_drm_mode_getfb2(owner, arg);
    case DRM_IOCTL_SYNCOBJ_EVENTFD:
        return gpu_syncobj_eventfd(owner, arg);
    case DRM_IOCTL_MODE_CLOSEFB: {
        return gpu_drm_mode_closefb(owner, arg);
    }
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return gpu_drm_create_dumb(owner, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:
    {
        struct drm_mode_map_dumb_compat req;
        struct fb_gpu_bo_entry *bo;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0)
            return -EINVAL;
        bo = fb_bo_get_owned(req.handle, owner->id, owner->tgid);
        if (bo == NULL)
            return -ENOENT;
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        fb_bo_put(bo);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map_compat req;
        uint64 size = 0;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.handle == 0)
            return -EINVAL;
        ret = virtio_gpu_user_resource_info(owner->id, owner->tgid,
                                            req.handle, NULL, NULL, NULL,
                                            &size);
        if (ret != 0) {
            struct fb_gpu_bo_entry *bo =
                fb_bo_get_owned(req.handle, owner->id, owner->tgid);
            if (bo == NULL)
                return ret;
            fb_bo_put(bo);
        }
        req.offset = GPU_DRM_MMAP_OFFSET(req.handle);
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_MODE_DESTROY_DUMB: {
        struct drm_mode_destroy_dumb_compat req;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        return fb_bo_destroy_owned(req.handle, owner->id, owner->tgid);
    }
    case DRM_IOCTL_VIRTGPU_GETPARAM: {
        struct drm_virtgpu_getparam_compat req;
        uint64 value = 0;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        switch (req.param) {
        case VIRTGPU_PARAM_3D_FEATURES:
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
        case VIRTGPU_PARAM_CONTEXT_INIT:
        case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
            value = virtio_gpu_has_virgl() ? 1 : 0;
            break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
            if (virtio_gpu_user_capset_ids(&value) != 0)
                value = 0;
            break;
        case VIRTGPU_PARAM_RESOURCE_BLOB:
        case VIRTGPU_PARAM_HOST_VISIBLE:
            value = 0;
            break;
        default:
            value = 0;
            break;
        }
        if (req.value == 0)
            return -EFAULT;
        if (either_copyout(1, req.value, &value, sizeof(value)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT: {
        struct drm_virtgpu_context_init_compat req;
        struct drm_virtgpu_context_set_param_compat param;
        char debug_name[64];
        uint32 capset_id = 0;
        uint32 current_capset = 0;
        uint32 i;
        int ret;

        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        if (req.pad != 0 || req.num_params > 16)
            return -EINVAL;

        ret = gpu_drm_current_capset(&current_capset);
        if (ret != 0)
            return ret;
        capset_id = current_capset;
        memcpy(debug_name, "xv6-drm", sizeof("xv6-drm"));

        if (req.num_params != 0 && req.ctx_set_params == 0)
            return -EFAULT;
        for (i = 0; i < req.num_params; i++) {
            uint64 user_param = req.ctx_set_params +
                (uint64)i * sizeof(param);
            if (either_copyin(&param, 1, user_param, sizeof(param)) < 0)
                return -EFAULT;

            switch (param.param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
                if (param.value == 0)
                    return -EINVAL;
                ret = virtio_gpu_user_get_caps_for((uint32)param.value, 0,
                                                   NULL, 0, NULL, NULL, NULL);
                if (ret != 0)
                    return -EINVAL;
                capset_id = (uint32)param.value;
                break;
            case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
                ret = gpu_user_debug_name(param.value, debug_name);
                if (ret != 0)
                    return ret;
                break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
                if (param.value > 1)
                    return -EINVAL;
                break;
            case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
                if (param.value > 1)
                    return -EINVAL;
                break;
            default:
                return -EINVAL;
            }
        }
        return gpu_owner_create_context(owner, debug_name, capset_id);
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
        return gpu_drm_virtgpu_resource_create(owner, arg, 0);
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB:
        return gpu_drm_virtgpu_resource_create(owner, arg, 1);
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info_compat req;
        uint64 size = 0;
        int ret;
        if (owner != NULL && owner->nouveau_channel != 0 &&
            gpu_nouveau_device() != NULL)
            return gpu_nouveau_notifier_alloc(owner, arg);
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = virtio_gpu_user_resource_info(owner->id, owner->tgid,
                                            req.bo_handle, NULL, NULL, NULL,
                                            &size);
        if (ret != 0)
            return ret;
        req.res_handle = req.bo_handle;
        req.size = (uint32)size;
        req.blob_mem = 0;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
        return gpu_drm_virtgpu_transfer(owner, arg, 1);
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
        return gpu_drm_virtgpu_transfer(owner, arg, 0);
    case DRM_IOCTL_VIRTGPU_WAIT: {
        struct drm_virtgpu_3d_wait_compat req;
        uint64 signaled = 0;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        return virtio_gpu_user_fence(0, !(req.flags & VIRTGPU_WAIT_NOWAIT),
                                     &signaled);
    }
    case DRM_IOCTL_VIRTGPU_GET_CAPS: {
        struct drm_virtgpu_get_caps_compat req;
        uint32 capset_id = 0, capset_ver = 0, capset_size = 0;
        uint32 copy_size;
        void *caps = NULL;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;

        ret = virtio_gpu_user_get_caps_for(req.cap_set_id, req.cap_set_ver,
                                           NULL, 0, &capset_id, &capset_ver,
                                           &capset_size);
        if (ret == -ENODEV) {
            capset_id = 0;
            capset_ver = 0;
            capset_size = 0;
            ret = 0;
        }
        if (ret != 0)
            return ret;

        if (req.addr != 0 && req.size != 0) {
            caps = kalloc();
            if (caps == NULL)
                return -ENOMEM;
        }
        if (caps != NULL)
            ret = virtio_gpu_user_get_caps_for(capset_id, capset_ver, caps,
                                               req.size, NULL, NULL, NULL);
        copy_size = capset_size;
        if (copy_size > req.size)
            copy_size = req.size;
        if (ret == 0 && caps != NULL && copy_size != 0 &&
            either_copyout(1, req.addr, caps, copy_size) < 0)
            ret = -EFAULT;
        if (caps != NULL)
            kfree(caps);
        if (ret != 0)
            return ret;
        req.cap_set_id = capset_id;
        req.cap_set_ver = capset_ver;
        req.size = capset_size;
        if (either_copyout(1, arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }
    case DRM_IOCTL_VIRTGPU_EXECBUFFER: {
        struct drm_virtgpu_execbuffer_compat req;
        uint32 *cmds;
        uint64 fence = 0, signaled = 0;
        int ret;
        if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
            return -EFAULT;
        ret = gpu_owner_ensure_context(owner);
        if (ret != 0)
            return ret;
        if (req.command == 0 || req.size == 0 || req.size > PGSIZE * 64 ||
            (req.size & 3) != 0)
            return -EINVAL;
        cmds = kvmalloc(req.size);
        if (cmds == NULL)
            return -ENOMEM;
        if (either_copyin(cmds, 1, req.command, req.size) < 0) {
            kvfree(cmds);
            return -EFAULT;
        }
        ret = virtio_gpu_user_submit(owner->id, owner->tgid,
                                     owner->default_ctx_id, 0, cmds,
                                     req.size / sizeof(uint32), NULL, 0,
                                     &fence, &signaled);
        kvfree(cmds);
        return ret;
    }
    case DRM_IOCTL_NOUVEAU_GETPARAM:
        return gpu_nouveau_getparam(arg);
    case DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC:
        return gpu_nouveau_channel_alloc(owner, arg);
    case DRM_IOCTL_NOUVEAU_CHANNEL_FREE:
        return gpu_nouveau_channel_free(owner, arg);
    case DRM_IOCTL_NOUVEAU_GROBJ_ALLOC:
        return gpu_nouveau_grobj_alloc(owner, arg);
    case DRM_IOCTL_NOUVEAU_GPUOBJ_FREE:
        return gpu_nouveau_gpuobj_free(owner, arg);
    case DRM_IOCTL_NOUVEAU_NVIF:
        return gpu_nouveau_nvif(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_NEW:
        return gpu_nouveau_gem_new(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_INFO:
        return gpu_nouveau_gem_info(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_CPU_PREP:
        return gpu_nouveau_cpu_prep(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_CPU_FINI:
        return gpu_nouveau_cpu_fini(owner, arg);
    case DRM_IOCTL_NOUVEAU_GEM_PUSHBUF:
        return gpu_nouveau_pushbuf(owner, arg);
    case DRM_IOCTL_NOUVEAU_VM_INIT:
        return gpu_nouveau_vm_init(owner, arg);
    case DRM_IOCTL_NOUVEAU_VM_BIND:
        return gpu_nouveau_bind_or_exec(owner, arg, 0);
    case DRM_IOCTL_NOUVEAU_EXEC:
        return gpu_nouveau_bind_or_exec(owner, arg, 1);
    default:
        return -EINVAL;
    }
}

#define GPU_DRM_IOCTL_DESC(_cmd, _flags) \
    { (_cmd), #_cmd, (_flags), gpu_drm_ioctl_handle }

static const struct drm_core_ioctl_desc gpu_drm_ioctls[] = {
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VERSION, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_UNIQUE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_MAGIC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_MAP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_CLIENT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_STATS, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_VERSION, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_CAP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_CLIENT_CAP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_CLIENT_NAME, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AUTH_MAGIC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_ADD_MAP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_ADD_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MARK_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_INFO_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MAP_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_FREE_BUFS,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_RM_MAP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_SAREA_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_SAREA_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SET_MASTER,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_DROP_MASTER,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_ADD_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_RM_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MOD_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GET_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SWITCH_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NEW_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_RES_CTX,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_DMA,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_LOCK,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_UNLOCK,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_FINISH,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GEM_CLOSE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GEM_FLINK, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_GEM_OPEN, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_PRIME_HANDLE_TO_FD, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_PRIME_FD_TO_HANDLE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_ACQUIRE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_RELEASE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_ENABLE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_INFO,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_ALLOC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_FREE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_BIND,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_AGP_UNBIND,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SG_ALLOC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SG_FREE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_WAIT_VBLANK,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_CRTC_GET_SEQUENCE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_CRTC_QUEUE_SEQUENCE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETRESOURCES,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETCRTC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_SETCRTC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CURSOR,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETGAMMA,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_SETGAMMA,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETENCODER,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETCONNECTOR,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPROPERTY,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPROPBLOB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_ADDFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_RMFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_PAGE_FLIP,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_DIRTYFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CREATE_DUMB, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_MAP_DUMB, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_DESTROY_DUMB, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPLANERESOURCES,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETPLANE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_SETPLANE,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_ADDFB2,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_OBJ_GETPROPERTIES,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_OBJ_SETPROPERTY,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CURSOR2,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_ATOMIC,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CREATEPROPBLOB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_DESTROYPROPBLOB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CREATE_LEASE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_LIST_LESSEES,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GET_LEASE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_REVOKE_LEASE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_CREATE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_DESTROY, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_XV6_SYNCOBJ_HANDLE_TO_FD_LEGACY,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_XV6_SYNCOBJ_FD_TO_HANDLE_LEGACY,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_WAIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_RESET, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_SIGNAL, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_QUERY, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_TRANSFER, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_GETFB2,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_SYNCOBJ_EVENTFD, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_MODE_CLOSEFB,
                       DRM_CORE_IOCTL_LEGACY | DRM_CORE_IOCTL_PRIMARY),

    /* Driver-private virtio-gpu commands. */
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_MAP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_EXECBUFFER, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_GETPARAM, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_RESOURCE_INFO, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_WAIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_GET_CAPS, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
                       DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_VIRTGPU_CONTEXT_INIT, DRM_CORE_IOCTL_ANY),

    /* Driver-private Nouveau commands. */
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GETPARAM, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_CHANNEL_FREE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GROBJ_ALLOC, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GPUOBJ_FREE, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_NVIF, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_VM_INIT, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_VM_BIND, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_EXEC, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_NEW, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_PUSHBUF, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_CPU_PREP, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_CPU_FINI, DRM_CORE_IOCTL_ANY),
    GPU_DRM_IOCTL_DESC(DRM_IOCTL_NOUVEAU_GEM_INFO, DRM_CORE_IOCTL_ANY),
};

static int gpu_drm_ioctl(struct fb_gpu_render_owner *owner, uint64 cmd,
                         uint64 arg)
{
    const char *name = NULL;
    int known = 0;
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (owner->drm.node_type == DRM_CORE_NODE_PRIMARY ||
        owner->drm.node_type == DRM_CORE_NODE_RENDER) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_ioctls++;
        spin_unlock(&fb_state.lock);
    }
    ret = drm_core_dispatch_ioctl(&owner->drm, owner, cmd, arg,
                                  gpu_drm_ioctls,
                                  sizeof(gpu_drm_ioctls) /
                                      sizeof(gpu_drm_ioctls[0]),
                                  &name, &known);
    if (!known) {
        spin_lock(&fb_state.lock);
        fb_state.stats.drm_unknown_ioctls++;
        spin_unlock(&fb_state.lock);
        printf("DRM: unknown ioctl node=%s owner=%lu tgid=%d cmd=0x%lx\n",
               drm_core_node_name(owner->drm.node_type),
               owner->id, owner->tgid, cmd);
    } else if (known == 2) {
        printf("DRM: denied ioctl node=%s owner=%lu tgid=%d cmd=0x%lx(%s)\n",
               drm_core_node_name(owner->drm.node_type),
               owner->id, owner->tgid, cmd, name ? name : "?");
    }
    return ret;
}

static int gpu_fops_release(struct vfs_inode *inode, struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    (void)inode;
    if (file != NULL)
        file->private_data = NULL;
    if (owner != NULL) {
        uint64 stale_events;
        uint64 stale_kms_fbs;
        uint64 stale_syncobjs;
        uint64 stale_gem_handles;
        enum drm_core_node_type node_type = owner->drm.node_type;

        spin_lock(&fb_state.lock);
        stale_events = gpu_drm_event_release_stale_locked(owner);
        spin_unlock(&fb_state.lock);
        drm_core_release_file(&owner->drm);
        fb_dxg_present_release_owner_sources(owner->id, owner->tgid);
        (void)gpu_nouveau_destroy_owner(owner);
        stale_kms_fbs = gpu_kms_destroy_owner_fbs(owner);
        stale_syncobjs = gpu_syncobj_destroy_owner(owner);
        stale_gem_handles = fb_gpu_count_render_owner_bos(owner->id);
        fb_gpu_destroy_render_owner(owner->id);
        fb_gpu_render_owner_unregister(owner);
        virtio_gpu_user_destroy_render_owner(owner->id);
        gpu_drm_lifecycle_live_close(owner);
        gpu_drm_lifecycle_close(node_type, stale_gem_handles, stale_kms_fbs,
                                stale_syncobjs, stale_events);
        (void)gpu_release_node(node_type);
        kvfree(owner);
        return 0;
    }
    return gpu_release(&gpu_cdev);
}

static void gpu_fops_last_fd_close(struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    gpu_drm_lifecycle_live_close(owner);
}

static void gpu_fops_first_fd_open(struct vfs_file *file)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    gpu_drm_lifecycle_live_open(owner);
}

static int gpu_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (trace)
        printf("fb-gpu-trace: enter pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd));
    switch (cmd) {
    case FB_GPU_GET_STATS:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_PRESENT:
    case FB_GPU_PAGE_FLIP:
    case FB_GPU_BO_COPY:
    case FB_GPU_SET_CURSOR:
    case FB_GPU_MOVE_CURSOR:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_TEST_DMABUF_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_TTM_VALIDATE:
    case FB_GPU_DXG_PRESENT_SOURCE_REGISTER:
    case FB_GPU_DXG_PRESENT_SOURCE_COMMIT:
    case FB_GPU_DXG_PRESENT_SOURCE_QUERY:
    case FB_GPU_DXG_PRESENT_BIND_CONTRACT_QUERY:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_ATTACH:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BACKEND_QUERY:
        break;
    default:
        ret = gpu_drm_ioctl(owner, cmd, (uint64)arg);
        if (trace)
            printf("fb-gpu-trace: exit pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
                   current ? current->pid : -1,
                   current ? current->name : "?", owner->id, owner->tgid, cmd,
                   fb_gpu_ioctl_name(cmd), ret);
        return ret;
    }

    spin_lock(&fb_state.lock);
    fb_state.stats.gpu_ioctls++;
    spin_unlock(&fb_state.lock);
    ret = fb_ioctl_for_owner(&gpu_cdev, cmd, arg, owner->id, owner->tgid);
    if (trace)
        printf("fb-gpu-trace: exit pid=%d name=%s owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static ssize_t gpu_fops_read(struct vfs_file *file, char *buf, size_t count,
                             bool user)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    struct drm_event_vblank_compat ev;
    uint32 old_head;

    if (owner == NULL || buf == NULL)
        return -EBADF;
    spin_lock(&fb_state.lock);
    if (owner->drm_event_count == 0) {
        spin_unlock(&fb_state.lock);
        return -EAGAIN;
    }
    old_head = owner->drm_event_head;
    ev = owner->drm_events[old_head];
    spin_unlock(&fb_state.lock);
    if (count < ev.base.length)
        return -EINVAL;
    if (either_copyout(user ? 1 : 0, (uint64)buf, &ev, sizeof(ev)) < 0)
        return -EFAULT;

    spin_lock(&fb_state.lock);
    if (owner->drm_event_count != 0 && owner->drm_event_head == old_head) {
        owner->drm_event_head =
            (owner->drm_event_head + 1) % FB_GPU_DRM_EVENT_QUEUE_CAPACITY;
        owner->drm_event_count--;
        if (fb_state.stats.drm_event_queue_depth != 0)
            fb_state.stats.drm_event_queue_depth--;
        fb_state.stats.drm_events_read++;
    }
    spin_unlock(&fb_state.lock);
    return sizeof(ev);
}

static int gpu_fops_poll(struct vfs_file *file, short events)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;

    if (owner == NULL)
        return POLLERR | POLLHUP;
    spin_lock(&fb_state.lock);
    if (owner->drm_event_count != 0) {
        spin_unlock(&fb_state.lock);
        return events & (POLLIN | POLLRDNORM | POLLRDBAND);
    }
    spin_unlock(&fb_state.lock);
    return 0;
}

static void *gpu_fops_fault(struct vfs_file *file, struct vma *vma, uint64 va)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    uint64 off;
    uint32 handle;
    uint64 page_index;
    void *pa;

    if (owner == NULL || vma == NULL || va < vma->start)
        return NULL;

    off = vma->pgoff + (va - vma->start);
    handle = GPU_DRM_MMAP_HANDLE(off);
    page_index = GPU_DRM_MMAP_PAGE(off);
    if (handle == 0)
        return NULL;

    pa = fb_bo_page_for_owner(handle, owner->id, owner->tgid, page_index);
    if (pa != NULL)
        return pa;
    return virtio_gpu_user_resource_page(owner->id, owner->tgid, handle,
                                         page_index);
}

static struct vfs_file_ops gpu_file_ops = {
    .read    = gpu_fops_read,
    .release = gpu_fops_release,
    .ioctl   = gpu_fops_ioctl,
    .poll    = gpu_fops_poll,
    .fault   = gpu_fops_fault,
    .first_fd_open = gpu_fops_first_fd_open,
    .last_fd_close = gpu_fops_last_fd_close,
};

static int gpu_drm_private_render_ioctl_allowed(uint64 cmd)
{
    switch (cmd) {
    case FB_GPU_BACKEND_QUERY:
    case FB_GPU_DISPLAY_PROBE:
    case FB_GPU_BO_CREATE:
    case FB_GPU_BO_DESTROY:
    case FB_GPU_BO_COPY:
    case FB_GPU_BO_IMPORT:
    case FB_GPU_BO_EXPORT_FD:
    case FB_GPU_TEST_DMABUF_EXPORT_FD:
    case FB_GPU_BO_IMPORT_FD:
    case FB_GPU_BO_INFO:
    case FB_GPU_BO_FENCE:
    case FB_GPU_FENCE_EXPORT_FD:
    case FB_GPU_FENCE_QUERY:
    case FB_GPU_VIRGL_CTX_CREATE:
    case FB_GPU_VIRGL_CTX_DESTROY:
    case FB_GPU_VIRGL_SUBMIT:
    case FB_GPU_VIRGL_FENCE:
    case FB_GPU_VIRGL_FENCE_EXPORT_FD:
    case FB_GPU_VIRGL_FENCE_QUERY_FD:
    case FB_GPU_VIRGL_GET_CAPS:
    case FB_GPU_VIRGL_RESOURCE_CREATE:
    case FB_GPU_VIRGL_RESOURCE_DESTROY:
    case FB_GPU_VIRGL_RESOURCE_ATTACH:
    case FB_GPU_VIRGL_RESOURCE_EXPORT_FD:
    case FB_GPU_VIRGL_TRANSFER_TO_HOST:
    case FB_GPU_VIRGL_TRANSFER_FROM_HOST:
        return 1;
    default:
        return 0;
    }
}

static int gpu_drm_fops_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct fb_gpu_render_owner *owner =
        file ? (struct fb_gpu_render_owner *)file->private_data : NULL;
    int trace = fb_gpu_trace_enabled() && fb_gpu_trace_process();
    int ret;

    if (owner == NULL)
        return -EBADF;
    if (trace)
        printf("fb-gpu-trace: enter pid=%d name=%s drm-owner=%lu:%d cmd=0x%lx(%s)\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd));

    if (gpu_drm_private_render_ioctl_allowed(cmd)) {
        /*
         * Mesa's xv6 virgl winsys opens the Linux render node, then issues the
         * xv6 driver-private virgl/BO ioctls that back that winsys.  Keep that
         * render-private ABI available here, while display-present/blit,
         * scanout, and Hyper-V present-source commands stay on the fb/gpu
         * compatibility devices.
         */
        spin_lock(&fb_state.lock);
        fb_state.stats.gpu_ioctls++;
        spin_unlock(&fb_state.lock);
        ret = fb_ioctl_for_owner(&gpu_cdev, cmd, arg, owner->id,
                                 owner->tgid);
        if (trace)
            printf("fb-gpu-trace: exit pid=%d name=%s drm-owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
                   current ? current->pid : -1,
                   current ? current->name : "?", owner->id, owner->tgid, cmd,
                   fb_gpu_ioctl_name(cmd), ret);
        return ret;
    }

    ret = gpu_drm_ioctl(owner, cmd, (uint64)arg);
    if (trace)
        printf("fb-gpu-trace: exit pid=%d name=%s drm-owner=%lu:%d cmd=0x%lx(%s) ret=%d\n",
               current ? current->pid : -1,
               current ? current->name : "?", owner->id, owner->tgid, cmd,
               fb_gpu_ioctl_name(cmd), ret);
    return ret;
}

static struct vfs_file_ops gpu_drm_file_ops = {
    .read    = gpu_fops_read,
    .release = gpu_fops_release,
    .ioctl   = gpu_drm_fops_ioctl,
    .poll    = gpu_fops_poll,
    .fault   = gpu_fops_fault,
    .first_fd_open = gpu_fops_first_fd_open,
    .last_fd_close = gpu_fops_last_fd_close,
};

static int gpu_open_file_common(cdev_t *cdev, struct vfs_file *file,
                                struct vfs_file_ops *ops)
{
    struct fb_gpu_render_owner *owner;
    int ret = gpu_open(cdev);

    if (ret != 0)
        return ret;
    owner = kvmalloc(sizeof(*owner));
    if (owner == NULL) {
        (void)gpu_release(cdev);
        return -ENOMEM;
    }
    memset(owner, 0, sizeof(*owner));
    owner->id = gpu_alloc_render_owner_id();
    owner->tgid = current ? current->tgid : 0;
    owner->next_bo_handle = 1;
    drm_core_file_init(&fb_drm_device, &owner->drm,
                       gpu_drm_node_from_cdev(cdev),
                       owner->id, owner->tgid);
    ret = fb_gpu_render_owner_register(owner);
    if (ret != 0) {
        kvfree(owner);
        (void)gpu_release(cdev);
        return ret;
    }
    file->ops = ops;
    file->private_data = owner;
    gpu_drm_lifecycle_open(owner->drm.node_type);
    owner->lifecycle_live_accounted = 1;
    printf("DRM: open node=%s owner=%lu tgid=%d magic=%u\n",
           drm_core_node_name(owner->drm.node_type), owner->id, owner->tgid,
           owner->drm.magic);
    return 0;
}

static int gpu_open_file(cdev_t *cdev, struct vfs_file *file)
{
    return gpu_open_file_common(cdev, file, &gpu_file_ops);
}

static int gpu_drm_open_file(cdev_t *cdev, struct vfs_file *file)
{
    return gpu_open_file_common(cdev, file, &gpu_drm_file_ops);
}

static cdev_t fb_cdev = {
    .dev = {
        .major = FB_MAJOR,
        .minor = FB_MINOR,
        .devname = "fb0",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .read    = fb_read,
        .write   = fb_write,
        .open    = fb_open,
        .release = fb_release,
