
static struct {
    int global_present;
    int vgpu_present;
    int vgpu_count;
    int global_gpadl_ok;
    int vgpu_gpadl_ok;
    int global_open_ok;
    int vgpu_open_ok;
    int cdev_registered;
    int read_emitted;
    int global_monitor_allocated;
    int vgpu_monitor_allocated;
    int global_dedicated;
    int vgpu_dedicated;
    size_t read_offset;
    uint32 global_relid;
    uint32 global_conn_id;
    uint32 vgpu_relid;
    uint32 vgpu_conn_id;
    uint32 global_offer_user_def[4];
    uint32 vgpu_offer_user_def[4];
    uint32 global_gpadl_status;
    uint32 vgpu_gpadl_status;
    uint32 global_open_status;
    uint32 vgpu_open_status;
    uint32 global_mmio_megabytes;
    uint32 hyperv_pci_offer_present;
    uint32 hyperv_pci_offer_count;
    uint32 hyperv_pci_offer_relid;
    uint32 hyperv_pci_offer_conn_id;
    uint32 hyperv_pci_offer_monitorid;
    uint32 hyperv_pci_offer_monitor_allocated;
    uint32 hyperv_pci_offer_dedicated;
    uint32 hyperv_pci_offer_flags;
    uint32 hyperv_pci_offer_mmio_megabytes;
    uint32 hyperv_pci_offer_user_def[4];
    struct hv_guid hyperv_pci_offer_instance;
    int32 iospace_last_ret;
    uint32 iospace_last_len;
    int iospace_set;
    uint64 iospace_base;
    uint64 iospace_size;
    uint32 fence_map_last_source;
    uint32 fence_map_last_mode;
    uint32 fence_map_failures;
    uint64 fence_map_last_raw_pa;
    uint64 fence_map_last_canonical_pa;
    uint64 fence_map_last_offset;
    uint64 fence_map_last_offset_candidate_pa;
    uint64 fence_map_last_offset_candidate_current;
    uint64 fence_map_last_size;
    uint64 fence_map_last_user_va;
    uint64 fence_map_last_kva;
    uint32 fence_map_max_source;
    uint32 fence_map_max_mode;
    uint64 fence_map_max_raw_pa;
    uint64 fence_map_max_canonical_pa;
    uint64 fence_map_max_offset_candidate_pa;
    uint64 fence_map_max_offset_candidate_current;
    uint32 fence_value_max_seen;
    uint64 fence_value_last_kva;
    uint64 fence_value_last_current;
    uint64 fence_value_last_target;
    uint32 global_rx_packets;
    uint32 vgpu_rx_packets;
    uint8 global_monitorid;
    uint8 vgpu_monitorid;
    uint64 next_trans_id;
    uint64 waiting_trans_id;
    uint32 waiting_channel;
    uint32 waiting_relid;
    uint64 completion_trans_id;
    volatile int completion_pending;
    uint16 completion_type;
    uint32 completion_len;
    uint16 completion_desc_type;
    uint16 completion_desc_flags;
    uint32 completion_desc_len8;
    uint32 completion_desc_offset8;
    uint32 completion_packet_len;
    uint32 completion_packet_offset;
    uint32 completion_payload_len;
    uint64 completion_desc_trans_id;
    uint64 completion_waiting_trans_id;
    uint32 completion_source_channel;
    uint32 completion_source_relid;
    uint32 completion_waiting_channel;
    uint32 completion_waiting_relid;
    uint32 completion_waiting_match;
    uint32 completion_waiting_channel_match;
    uint8 completion_buf[HV_DXG_RESULT_BYTES];
    uint8 rx_buf[HV_DXG_PACKET_BYTES];
    volatile int pump_active;
    uint32 pump_skips;
    volatile int sync_active;
    uint32 sync_waits;
    uint32 sync_timeouts;
    mutex_t process_lock;
    struct hvdxg_process_state *processes[HV_DXG_PROCESS_TABLE_MAX];
    uint64 process_guest_next;
    uint32 process_generation;
    uint32 process_live;
    uint32 process_live_max;
    uint32 process_creates;
    uint32 process_reuses;
    uint32 process_releases;
    uint32 process_destroy_attempts;
    uint32 process_destroy_successes;
    uint32 process_destroy_failures;
    uint32 process_destroy_suppressed;
    uint32 process_destroy_active_total;
    uint32 process_destroy_active_device;
    uint32 process_destroy_active_context;
    uint32 process_destroy_active_hwqueue;
    uint32 process_destroy_active_pagingqueue;
    uint32 process_destroy_active_sync;
    uint32 process_destroy_active_allocation;
    uint32 process_destroy_active_resource;
    uint32 process_destroy_active_gpuva;
    uint32 process_destroy_deferred;
    uint32 process_shared_reuses;
    uint32 process_isolated_reuses;
    uint64 process_isolated_last_tgid;
    uint32 process_isolated_last_handle;
    uint32 process_isolated_last_generation;
    uint32 process_isolated_source_generation;
    uint32 process_isolated_copied_objects;
    uint32 process_isolated_source_objects;
    uint32 process_retained_reuse_avoided;
    uint64 process_retained_avoided_tgid;
    uint64 process_retained_avoided_source_tgid;
    uint32 process_retained_avoided_handle;
    uint32 process_retained_avoided_generation;
    uint32 process_retained_avoided_source_generation;
    uint64 process_namespace_last_tgid;
    uint32 process_namespace_last_handle;
    uint32 process_namespace_source_generation;
    uint32 process_namespace_new_generation;
    uint32 process_namespace_objects_before;
    uint32 process_namespace_objects_after;
    uint32 process_namespace_adapters_before;
    uint32 process_namespace_adapters_after;
    uint32 process_namespace_locals_before;
    uint32 process_namespace_locals_after;
    uint32 process_namespace_fresh;
    uint32 process_retained_handle;
    uint32 process_retained_generation;
    uint64 process_retained_tgid;
    uint32 process_retained_refs;
    uint32 process_table_full;
    struct hvdxg_deferred_shared_destroy
        deferred_shared_destroy[HV_DXG_DEFERRED_SHARED_DESTROY_MAX];
    uint32 deferred_shared_destroy_queued;
    uint32 deferred_shared_destroy_flushed;
    uint32 deferred_shared_destroy_failed;
    int32 deferred_shared_destroy_last_ret;
    uint32 deferred_shared_destroy_last_process;
    uint32 deferred_shared_destroy_last_object;
    uint32 deferred_shared_destroy_last_nt;
    uint32 deferred_shared_destroy_last_device;
    uint32 deferred_shared_destroy_last_resource;
    uint64 host_event_next_id;
    uint64 host_event_ids[HV_DXG_HOST_EVENT_MAX];
    struct vfs_file *host_event_files[HV_DXG_HOST_EVENT_MAX];
    uint8 host_event_signaled[HV_DXG_HOST_EVENT_MAX];
    uint8 host_event_remove_after_signal[HV_DXG_HOST_EVENT_MAX];
    uint64 host_event_last_id;
    uint32 host_event_signal_count;
    uint32 host_event_wait_successes;
    uint32 host_event_wait_timeouts;
    uint32 host_event_wait_failures;
    uint32 probe_attempts;
    uint32 probe_successes;
    int32 probe_last_ret;
    int32 probe_open_status;
    uint32 probe_open_handle;
    uint32 probe_open_requested_version;
    uint32 probe_open_host_version;
    uint32 probe_open_host_compat;
    uint32 probe_info_len;
    uint32 probe_info_flags;
    uint32 probe_async_msg_enabled;
    int32 probe_v40_open_send_ret;
    uint32 probe_v40_open_actual_len;
    int32 probe_v40_open_status;
    uint32 probe_v40_open_handle;
    uint32 probe_v40_open_host_version;
    uint32 probe_v40_open_host_compat;
    uint32 probe_v40_open_guest_luid_low;
    uint32 probe_v40_open_guest_luid_high;
    int32 probe_v40_getinternal_send_ret;
    uint32 probe_v40_getinternal_actual_len;
    uint32 probe_v40_getinternal_flags;
    uint32 probe_v40_reject_reason;
    int d3dkmt_ready;
    uint64 dxg_process_guest;
    uint64 dxg_process_pid;
    uint64 dxg_process_tgid;
    uint32 dxg_process_generation;
    uint32 createprocess_last_len;
    uint32 createprocess_last_cmd_len;
    int32 createprocess_last_ret;
    uint64 createprocess_last_guest;
    uint64 createprocess_last_pid;
    uint64 createprocess_last_tgid;
    uint32 createprocess_last_handle;
    uint32 createprocess_last_layout;
    uint32 createprocess_last_generation;
    uint32 createprocess_success_len;
    uint32 createprocess_success_cmd_len;
    int32 createprocess_success_ret;
    uint64 createprocess_success_guest;
    uint64 createprocess_success_pid;
    uint64 createprocess_success_tgid;
    uint32 createprocess_success_handle;
    uint32 createprocess_success_layout;
    uint32 createprocess_success_generation;
    uint32 open_createprocess_attempts;
    uint32 open_createprocess_successes;
    uint32 open_createprocess_failures;
    uint32 open_createprocess_ignored_failures;
    int32 open_createprocess_last_ret;
    uint64 open_createprocess_last_guest;
    uint64 open_createprocess_last_pid;
    uint64 open_createprocess_last_tgid;
    uint32 open_createprocess_last_handle;
    uint32 open_createprocess_last_created;
    uint32 open_createprocess_last_generation;
    uint32 open_createprocess_last_refs;
    uint32 early_bind_attempts;
    uint32 early_bind_successes;
    uint32 early_bind_failures;
    uint32 early_bind_last_source;
    uint32 early_bind_last_cmd;
    int32 early_bind_last_ret;
    uint32 early_bind_last_handle;
    uint32 early_bind_last_created;
    uint32 early_bind_last_generation;
    uint32 early_bind_last_refs;
    uint32 destroyprocess_last_len;
    int32 destroyprocess_last_ret;
    uint32 destroyprocess_last_handle;
    uint32 host_cmd_destroyallocation;
    uint32 host_cmd_destroycontext;
    uint32 host_cmd_destroyhwqueue;
    uint32 host_cmd_destroypagingqueue;
    uint32 host_cmd_destroydevice;
    uint32 host_cmd_destroysync;
    uint32 host_cmd_destroyprocess;
    uint32 host_cmd_freegpuva;
    uint32 host_cmd_lock2;
    uint32 host_cmd_unlock2;
    uint32 closeadapter_ioctl_count;
    uint32 closeadapter_local_count;
    uint32 closeadapter_host_count;
    uint32 closeadapter_invalid_count;
    uint32 closeadapter_last_len;
    int32 closeadapter_last_ret;
    int32 closeadapter_last_status;
    uint32 cleanup_order_seq;
    uint32 cleanup_last_destroy_order;
    uint32 cleanup_destroyprocess_order;
    uint32 closeadapter_last_order;
    uint32 closeadapter_after_destroy_count;
    uint32 device_state_counter;
    uint32 ioctl_count;
    uint32 ioctl_successes;
    int32 ioctl_last_ret;
    uint32 ioctl_history_index;
    uint32 ioctl_history_cmd[HV_DXG_IOCTL_HISTORY_MAX];
    uint32 ioctl_history_nr[HV_DXG_IOCTL_HISTORY_MAX];
    int32 ioctl_history_ret[HV_DXG_IOCTL_HISTORY_MAX];
    uint32 ioctl_nr_count[HV_DXG_IOCTL_NR_MAX];
    uint64 ioctl_nr_total_ticks[HV_DXG_IOCTL_NR_MAX];
    uint64 ioctl_nr_last_ticks[HV_DXG_IOCTL_NR_MAX];
    uint64 ioctl_nr_max_ticks[HV_DXG_IOCTL_NR_MAX];
    uint32 open_count;
    uint32 live_open_count;
    uint32 cleanup_attempts;
    uint32 cleanup_successes;
    int32 cleanup_last_ret;
    uint32 cleanup_last_op;
    uint32 cleanup_last_handle;
    uint32 cleanup_failed_op;
    uint32 cleanup_failed_handle;
    uint32 cleanup_had_tracked;
    uint32 cleanup_resource_host_destroys;
    uint32 cleanup_resource_child_locals;
    uint32 cleanup_standalone_alloc_destroys;
    uint32 cleanup_resource_alloc_skips;
    uint32 object_table_max;
    uint32 object_table_drops;
    uint32 object_table_denied;
    uint32 object_table_generation;
    uint32 object_table_reuse_delayed;
    uint32 object_table_reuse_allowed;
    uint32 object_table_min_free_entries;
    uint32 object_table_free_count;
    uint32 object_table_free_head;
    uint32 object_table_free_tail;
    uint32 track_allocation_max;
    uint32 track_allocation_drops;
    uint32 track_gpuva_max;
    uint32 track_gpuva_drops;
    uint32 track_hwqueue_max;
    uint32 track_hwqueue_drops;
    uint32 track_pagingqueue_max;
    uint32 track_pagingqueue_drops;
    uint32 pagingqueue_last_len;
    int32 pagingqueue_last_ret;
    uint32 pagingqueue_last_queue;
    uint32 pagingqueue_last_sync;
    uint64 pagingqueue_last_fence_pa;
    uint64 pagingqueue_last_fence_off;
    uint32 gpuva_reserve_last_len;
    int32 gpuva_reserve_last_ret;
    uint64 gpuva_reserve_last_va;
    uint64 gpuva_reserve_last_fence;
    uint32 gpuva_free_last_len;
    int32 gpuva_free_last_ret;
    uint32 gpuva_free_last_adapter;
    uint64 gpuva_free_last_base;
    uint64 gpuva_free_last_size;
    uint64 gpuva_free_last_wire_size;
    uint32 syncobject_last_len;
    int32 syncobject_last_ret;
    uint32 syncobject_last_handle;
    uint32 syncobject_last_type;
    uint32 syncobject_last_flags;
    uint32 syncobject_last_global;
    uint32 syncobject_last_process;
    uint32 syncobject_last_owner_process;
    uint32 syncobject_last_owner_generation;
    uint64 syncobject_last_fence_cpu;
    uint64 syncobject_last_fence_gpu;
    uint64 syncobject_last_fence_pa;
    uint64 syncobject_last_fence_off;
    uint32 syncobject_mapped_count;
    uint32 syncobject_mapped_len;
    uint32 syncobject_mapped_type;
    uint32 syncobject_mapped_flags;
    uint64 syncobject_mapped_fence_cpu;
    uint64 syncobject_mapped_fence_gpu;
    uint32 syncsignal_last_len;
    int32 syncsignal_last_ret;
    int32 syncsignal_last_status;
    uint32 syncwait_last_len;
    int32 syncwait_last_ret;
    int32 syncwait_last_status;
    uint64 syncwait_last_event;
    uint32 syncwait_last_async;
    uint32 syncwait_last_object;
    uint64 syncwait_last_fence;
    uint64 syncwait_last_current;
    uint32 syncwait_last_result;
    uint32 syncgpu_signal_last_len;
    int32 syncgpu_signal_last_ret;
    int32 syncgpu_signal_last_status;
    uint32 syncgpu_wait_last_len;
    int32 syncgpu_wait_last_ret;
    int32 syncgpu_wait_last_status;
    uint32 syncgpu_wait_last_context;
    uint32 syncgpu_wait_last_object;
    uint32 syncgpu_wait_last_object_count;
    uint32 syncgpu_wait_last_object_type;
    uint32 syncgpu_wait_last_legacy;
    uint64 syncgpu_wait_last_fence;
    uint32 syncgpu_wait_last_cmd_len;
    uint32 createdevice_last_adapter;
    uint32 createdevice_last_host_adapter;
    uint32 createdevice_last_device;
    uint32 createdevice_adapter_equals_device;
    uint32 createdevice_host_adapter_equals_device;
    int32 createdevice_last_ret;
    uint32 createdevice_last_process;
    uint32 createdevice_last_owner_process;
    uint32 createdevice_last_owner_generation;
    uint32 createdevice_last_owner_refs;
    uint32 createdevice_object_found;
    uint32 createdevice_object_type;
    uint32 createdevice_object_local;
    uint32 createdevice_object_host;
    uint32 createdevice_object_parent;
    uint32 createdevice_object_device;
    uint32 createdevice_object_generation;
    uint32 createdevice_object_destroyed;
    uint32 last_device_handle;
    uint32 last_resource_handle;
    uint32 last_allocation_handle;
    uint32 last_allocation_device;
    uint64 last_allocation_size;
    uint32 allocation_last_len;
    int32 allocation_last_ret;
    uint32 allocation_last_count;
    uint32 allocation_last_cmd_len;
    uint32 allocation_last_hdr_size;
    uint32 allocation_last_prr_offset;
    uint32 allocation_last_make_resident_offset;
    uint32 allocation_last_allocinfo_offset;
    uint32 allocation_last_private_offset;
    uint32 allocation_last_result_min_len;
    uint32 allocation_last_result_len;
    uint32 allocation_last_result_flags_offset;
    uint32 allocation_last_result_resource_offset;
    uint32 allocation_last_result_global_offset;
    uint32 allocation_last_result_vgpu_offset;
    uint32 allocation_last_result_allocinfo_offset;
    uint32 allocation_last_result_allocinfo_size;
    uint32 allocation_last_result_head_len;
    uint8 allocation_last_result_head[64];
    uint32 allocation_last_wire_len;
    uint32 allocation_last_ext;
    uint32 allocation_last_ext_offset;
    uint32 allocation_last_route_global;
    int32 allocation_last_send_ret;
    uint32 allocation_last_process;
    uint32 allocation_last_owner_process;
    uint32 allocation_last_owner_generation;
    uint32 allocation_last_device;
    uint32 allocation_last_device_known;
    uint32 allocation_last_device_from_create;
    uint32 allocation_last_runtime_size;
    uint32 allocation_last_resource_priv_size;
    uint32 allocation_last_priv_size;
    uint32 allocation_last_flags;
    uint64 allocation_last_sysmem;
    uint64 allocation_last_priority;
    uint32 existing_sysmem_last_pages;
    int32 existing_sysmem_last_pin_ret;
    int32 existing_sysmem_last_set_ret;
    uint32 existing_sysmem_pin_successes;
    uint32 existing_sysmem_set_successes;
    uint64 existing_sysmem_total_pages;
    uint32 existing_sysmem_attempts;
    uint32 existing_sysmem_last_path;
    uint32 existing_sysmem_last_standard;
    uint32 existing_sysmem_last_writable;
    uint32 existing_sysmem_last_device;
    uint32 existing_sysmem_last_allocation;
    uint64 existing_sysmem_last_va;
    uint64 existing_sysmem_last_size;
    uint64 existing_sysmem_last_first_pfn;
    uint64 existing_sysmem_last_last_pfn;
    uint32 existing_sysmem_last_pfnmap_pages;
    uint32 existing_sysmem_pfnmap_successes;
    uint32 existing_sysmem_last_vram;
    uint64 existing_sysmem_last_vram_gpa;
    uint64 existing_sysmem_last_vram_size;
    uint32 existing_sysmem_share_stage;
    uint32 existing_sysmem_share_resource;
    uint32 existing_sysmem_share_global;
    uint32 existing_sysmem_share_flags;
    uint32 existing_sysmem_share_host_flags;
    uint32 existing_sysmem_share_metadata;
    uint32 existing_sysmem_share_sealed;
    uint32 existing_sysmem_share_nt;
    uint32 existing_sysmem_share_shareable;
    uint32 existing_sysmem_share_reason;
    uint32 existing_sysmem_share_alloc_count;
    uint32 existing_sysmem_share_runtime_priv;
    uint32 existing_sysmem_share_resource_priv;
    uint32 existing_sysmem_share_alloc_priv;
    uint32 existing_sysmem_share_total_priv;
    uint32 existing_sysmem_share_pfnmap_pages;
    uint32 existing_sysmem_share_vram;
    uint64 existing_sysmem_share_va;
    uint64 existing_sysmem_share_size;
    uint32 allocation_last_in_priv_head_len;
    uint8 allocation_last_in_priv_head[64];
    uint32 allocation_last_out_priv_head_len;
    uint8 allocation_last_out_priv_head[64];
    uint32 allocation_history_index;
    uint32 allocation_history_len[HV_DXG_RESOURCE_HISTORY_MAX];
    int32 allocation_history_ret[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_device[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_resource[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_allocation[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 allocation_history_size[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_count[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_priv[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_global_share[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 allocation_history_create_flags[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 d3d12_shared_event_seq;
    uint64 d3d12_shared_create_seq;
    uint64 d3d12_shared_first_nt_seq;
    uint32 d3d12_shared_alloc_seen;
    uint32 d3d12_shared_alloc_len;
    int32 d3d12_shared_alloc_ret;
    uint32 d3d12_shared_alloc_device;
    uint32 d3d12_shared_alloc_resource_in;
    uint32 d3d12_shared_alloc_resource_out;
    uint32 d3d12_shared_alloc_allocation;
    uint32 d3d12_shared_alloc_count;
    uint32 d3d12_shared_alloc_flags;
    uint32 d3d12_shared_runtime_d3d12_flags;
    uint32 d3d12_shared_alloc_global_share;
    uint32 d3d12_shared_allocinfo_offset;
    uint32 d3d12_shared_runtime_offset;
    uint32 d3d12_shared_resource_priv_offset;
    uint32 d3d12_shared_alloc_priv_offset;
    uint32 d3d12_shared_wire_flags;
    uint32 d3d12_shared_wire_make_resident;
    uint64 d3d12_shared_wire_rt_resource;
    uint32 d3d12_shared_result_flags;
    uint32 d3d12_shared_result_global_share;
    uint32 d3d12_shared_result_vgpu_flags;
    uint32 d3d12_shared_result_alloc_flags;
    uint32 d3d12_shared_result_min_len;
    uint32 d3d12_shared_result_len;
    uint32 d3d12_shared_result_flags_offset;
    uint32 d3d12_shared_result_resource_offset;
    uint32 d3d12_shared_result_global_offset;
    uint32 d3d12_shared_result_vgpu_offset;
    uint32 d3d12_shared_result_allocinfo_offset;
    uint32 d3d12_shared_result_allocinfo_size;
    uint32 d3d12_shared_result_head_len;
    uint32 d3d12_shared_result_flag_norm;
    uint32 d3d12_shared_result_flag_norm_reason;
    uint32 d3d12_shared_result_flag_candidate;
    uint32 d3d12_shared_result_flag_delta;
    uint32 d3d12_shared_track_shared;
    uint32 d3d12_shared_track_nt;
    uint32 d3d12_shared_track_metadata;
    uint32 d3d12_shared_track_sent_bytes;
    uint32 d3d12_shared_track_alloc_from_host;
    uint64 d3d12_shared_alloc_process;
    uint64 d3d12_shared_alloc_size;
    uint64 d3d12_shared_alloc_rt_resource;
    uint32 d3d12_shared_runtime_size;
    uint32 d3d12_shared_resource_priv_size;
    uint32 d3d12_shared_alloc_priv_size;
    uint32 d3d12_shared_alloc_out_priv_size;
    int32 d3d12_shared_runtime_user_copy_ret;
    uint32 d3d12_shared_runtime_user_mismatch;
    uint32 d3d12_shared_runtime_user_head_len;
    uint32 d3d12_shared_norm_seen;
    uint32 d3d12_shared_norm_applied;
    uint32 d3d12_shared_norm_reason;
    uint32 d3d12_shared_norm_magic0;
    uint32 d3d12_shared_norm_magic3;
    uint32 d3d12_shared_norm_pre_w4;
    uint32 d3d12_shared_norm_post_w4;
    uint32 d3d12_shared_norm_pre_w8;
    uint32 d3d12_shared_norm_post_w8;
    uint32 d3d12_shared_norm_runtime_applied;
    uint32 d3d12_shared_norm_width;
    uint32 d3d12_shared_norm_height;
    uint64 d3d12_shared_norm_pre_rt8;
    uint64 d3d12_shared_norm_post_rt8;
    uint64 d3d12_shared_norm_pre_rt10;
    uint64 d3d12_shared_norm_post_rt10;
    uint64 d3d12_shared_norm_pre_rt38;
    uint64 d3d12_shared_norm_post_rt38;
    uint64 d3d12_shared_norm_pre_rt50;
    uint64 d3d12_shared_norm_post_rt50;
    uint64 d3d12_shared_norm_pre_rt58;
    uint64 d3d12_shared_norm_post_rt58;
    uint32 d3d12_shared_runtime_head_len;
    uint32 d3d12_shared_resource_priv_head_len;
    uint32 d3d12_shared_alloc_priv_head_len;
    uint32 d3d12_shared_alloc_out_priv_head_len;
    uint8 d3d12_shared_runtime_user_head[HV_DXG_SHARED_ALLOC_HEAD_MAX];
    uint8 d3d12_shared_runtime_head[HV_DXG_SHARED_ALLOC_HEAD_MAX];
    uint8 d3d12_shared_resource_priv_head[HV_DXG_SHARED_ALLOC_HEAD_MAX];
    uint8 d3d12_shared_alloc_priv_head[HV_DXG_SHARED_ALLOC_HEAD_MAX];
    uint8 d3d12_shared_alloc_out_priv_head[HV_DXG_SHARED_ALLOC_HEAD_MAX];
    uint8 d3d12_shared_result_head[HV_DXG_SHARED_ALLOC_HEAD_MAX];
    uint32 destroyalloc_last_len;
    int32 destroyalloc_last_ret;
    int32 destroyalloc_last_status;
    uint32 destroyalloc_last_device;
    uint32 destroyalloc_last_resource;
    uint32 destroyalloc_last_allocation;
    uint32 destroyalloc_last_process;
    uint32 destroyalloc_last_context;
    uint32 destroyalloc_last_count;
    uint32 destroyalloc_d3d12_match_count;
    uint32 destroyalloc_d3d12_pending_match_count;
    uint32 destroyalloc_d3d12_last_match;
    uint64 destroyalloc_d3d12_last_seq;
    uint64 destroyalloc_d3d12_first_seq;
    uint32 destroyalloc_d3d12_first_context;
    uint32 destroyalloc_d3d12_first_match;
    uint32 destroyalloc_d3d12_first_pending;
    uint32 destroyalloc_d3d12_first_before_nt;
    uint32 destroyalloc_d3d12_last_before_nt;
    uint32 destroyalloc_d3d12_context_mask;
    uint32 destroydevice_last_len;
    int32 destroydevice_last_ret;
    int32 destroydevice_last_status;
    uint32 destroycontext_last_len;
    int32 destroycontext_last_ret;
    int32 destroycontext_last_status;
    uint32 destroypaging_last_len;
    int32 destroypaging_last_ret;
    int32 destroypaging_last_status;
    uint32 destroysync_last_len;
    int32 destroysync_last_ret;
    int32 destroysync_last_status;
    uint32 destroysync_last_handle;
    uint32 destroysync_last_device;
    uint32 destroysync_last_type;
    uint32 destroysync_last_flags;
    uint32 destroysync_last_global;
    uint32 destroysync_last_monitor_fence;
    uint32 destroysync_last_cmd_len;
    uint32 destroysync_last_wire_len;
    uint32 destroysync_last_ext;
    uint32 destroysync_last_ext_offset;
    uint32 syncobject_last_cmd_len;
    uint32 syncobject_last_result_len;
    uint32 syncobject_last_result_sync_offset;
    uint32 syncobject_last_result_global_offset;
    uint32 syncobject_last_result_fence_gpu_offset;
    uint32 syncobject_last_result_fence_pa_offset;
    uint32 syncobject_last_result_fence_off_offset;
    uint32 syncobject_last_result_head_len;
    uint8 syncobject_last_result_head[64];
    uint32 syncobject_last_args_offset;
    uint32 syncobject_last_client_hint_offset;
    uint32 syncobject_last_client_hint;
    uint32 syncobject_last_input_shared;
    uint32 sync_diag_prints;
    uint32 createalloc_unwind_attempts;
    uint32 createalloc_unwind_successes;
    int32 createalloc_unwind_last_ret;
    uint32 makeresident_last_len;
    int32 makeresident_last_ret;
    uint64 makeresident_last_fence;
    uint64 makeresident_last_trim;
    int32 makeresident_last_status;
    uint32 makeresident_last_device;
    uint32 makeresident_last_paging_queue;
    uint32 makeresident_last_flags;
    uint32 makeresident_last_count;
    uint32 makeresident_last_sorted;
    uint32 makeresident_last_cmd_len;
    uint32 makeresident_last_wsl_cmd_len;
    uint32 makeresident_last_result_len;
    uint32 makeresident_last_actual_len;
    int32 makeresident_last_host_ret;
    int32 makeresident_last_user_ret;
    uint32 makeresident_last_pending_ok;
    uint32 makeresident_last_in_alloc[4];
    uint32 makeresident_last_wire_alloc[4];
    uint32 makeresident_last_owner_ok_count;
    uint32 makeresident_last_tracked_count;
    uint32 makeresident_last_order_matches;
    uint32 makeresident_last_owner_dev[2];
    uint32 makeresident_last_owner_res[2];
    uint32 makeresident_last_owner_proc[2];
    uint32 makeresident_last_owner_gen[2];
    uint32 makeresident_last_owner_refs[2];
    uint32 makeresident_last_sync;
    uint64 makeresident_last_fence_current;
    uint32 makeresident_diag_prints;
    uint32 evict_last_len;
    int32 evict_last_ret;
    uint64 evict_last_trim;
    uint32 mapgpuva_last_len;
    int32 mapgpuva_last_ret;
    int32 mapgpuva_last_status;
    uint32 mapgpuva_last_paging_queue;
    uint32 mapgpuva_last_allocation;
    uint64 mapgpuva_last_base;
    uint64 mapgpuva_last_min;
    uint64 mapgpuva_last_max;
    uint64 mapgpuva_last_size_pages;
    uint64 mapgpuva_last_protection;
    uint64 mapgpuva_last_driver_protection;
    uint64 mapgpuva_last_va;
    uint64 mapgpuva_last_fence;
    uint32 mapgpuva_last_sync;
    uint64 mapgpuva_last_fence_current;
    uint32 mapgpuva_history_index;
    uint32 mapgpuva_history_len[HV_DXG_RESOURCE_HISTORY_MAX];
    int32 mapgpuva_history_ret[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 mapgpuva_history_status[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 mapgpuva_history_paging_queue[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 mapgpuva_history_allocation[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_pages[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_protection[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_driver_protection[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_va[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_fence[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 queryadapter_last_type;
    uint32 queryadapter_last_size;
    uint32 queryadapter_last_len;
    uint32 queryadapter_last_user_len;
    int32 queryadapter_last_ret;
    int32 queryadapter_last_status;
    uint32 queryadapter_last_layout;
    uint32 queryadapter_last_cmd_len;
    uint32 queryadapter_last_type_offset;
    uint32 queryadapter_last_size_offset;
    uint32 queryadapter_last_data_offset;
    uint32 queryadapter_last_adapter;
    uint32 queryadapter_last_host_adapter;
    uint32 queryadapter_last_resolve_source;
    uint32 queryadapter_last_owner_process;
    uint32 queryadapter_last_owner_generation;
    uint32 queryadapter_last_owner_refs;
    uint32 queryadapter_last_result_len;
    uint32 queryadapter_last_expected_wsl_len;
    uint32 queryadapter_last_process_source;
    uint32 queryadapter_last_adapter_object;
    uint32 queryadapter_last_adapter_object_host;
    uint32 queryadapter_last_adapter_object_owner;
    uint32 queryadapter_last_adapter_object_owner_generation;
    uint32 queryadapter_last_adapter_object_generation;
    uint32 queryadapter_local_namespace;
    uint32 queryadapter_process_adapter_refs;
    uint32 queryadapter_process_adapter_locals;
    uint32 queryadapter_process_adapter_generation;
    int32 queryadapter_type15_fail_ret;
    int32 queryadapter_type15_fail_status;
    uint32 queryadapter_type15_fail_process;
    uint32 queryadapter_type15_fail_route;
    uint32 queryadapter_type15_fail_ext_luid_low;
    uint32 queryadapter_type15_fail_ext_luid_high;
    uint32 queryadapter_adaptertype_rewrite_count;
    uint32 queryadapter_adaptertype_rewrite_type;
    uint32 queryadapter_adaptertype_rewrite_source;
    uint32 queryadapter_adaptertype_raw_value;
    uint32 queryadapter_adaptertype_wsl_value;
    uint32 queryadapter_adaptertype_cleared_bits;
    uint32 queryadapter_adaptertype_forced_bits;
    uint32 queryadapter_adaptertype_compute_only;
    uint32 queryadapter_adaptertype_last_type;
    uint32 queryadapter_adaptertype_last_source;
    uint32 queryadapter_adaptertype_last_raw_value;
    uint32 queryadapter_adaptertype_last_wsl_value;
    uint32 queryadapter_zero_success_type;
    uint32 queryadapter_zero_success_size;
    uint32 queryadapter_zero_success_count;
    uint32 queryadapter_zero_success_host_type;
    uint32 queryadapter_zero_success_host_len;
    int32 queryadapter_zero_success_host_ret;
    int32 queryadapter_zero_success_host_status;
    int32 queryadapter_zero_success_user_ret;
    uint32 queryadapter_packet_type;
    uint32 queryadapter_packet_size;
    uint32 queryadapter_packet_cmd_len;
    uint32 queryadapter_packet_wire_len;
    uint32 queryadapter_packet_ext;
    uint32 queryadapter_packet_ext_offset;
    uint32 queryadapter_packet_type_offset;
    uint32 queryadapter_packet_size_offset;
    uint32 queryadapter_packet_data_offset;
    uint32 queryadapter_packet_desc_size;
    uint32 queryadapter_packet_len;
    uint32 queryadapter_packet_aligned;
    uint32 queryadapter_packet_pad;
    uint32 queryadapter_packet_desc_off8;
    uint32 queryadapter_packet_desc_len8;
    uint32 queryadapter_packet_ring_total;
    uint32 queryadapter_packet_ext_len;
    uint8 queryadapter_packet_ext_bytes[HV_DXG_QUERYADAPTER_EXT_SNAPSHOT_BYTES];
    uint32 queryadapter_packet_cmdhdr_len;
    uint8 queryadapter_packet_cmdhdr[HV_DXG_QUERYADAPTER_CMDHDR_SNAPSHOT_BYTES];
    uint32 queryadapter_packet_priv_head_len;
    uint8 queryadapter_packet_priv_head[HV_DXG_QUERYADAPTER_PRIV_SNAPSHOT_BYTES];
    uint32 queryadapter_send_route;
    uint32 queryadapter_send_ext_luid_low;
    uint32 queryadapter_send_ext_luid_high;
    uint32 queryadapter_completion_desc_type;
    uint32 queryadapter_completion_desc_flags;
    uint32 queryadapter_completion_desc_len8;
    uint32 queryadapter_completion_desc_offset8;
    uint32 queryadapter_completion_packet_len;
    uint32 queryadapter_completion_packet_offset;
    uint32 queryadapter_completion_payload_len;
    uint64 queryadapter_completion_trans_id;
    uint64 queryadapter_completion_waiting_trans_id;
    uint32 queryadapter_completion_source_channel;
    uint32 queryadapter_completion_source_relid;
    uint32 queryadapter_completion_waiting_channel;
    uint32 queryadapter_completion_waiting_relid;
    uint32 queryadapter_completion_waiting_match;
    uint32 queryadapter_completion_waiting_channel_match;
    uint32 queryadapter_completion_type;
    uint32 queryadapter_completion_len;
    uint8 queryadapter_completion_prefix[8];
    uint32 queryadapter_type0_private_size;
    uint32 queryadapter_type0_private_hash;
    uint32 queryadapter_type0_private_head_len;
    uint8 queryadapter_type0_private_head[HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES];
    uint32 queryadapter_type0_private_tail_len;
    uint8 queryadapter_type0_private_tail[HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES];
    uint32 queryadapter_type0_primary_len;
    int32 queryadapter_type0_primary_ret;
    int32 queryadapter_type0_primary_status;
    uint32 queryadapter_type0_fallback_attempted;
    uint32 queryadapter_type0_fallback_used;
    uint32 queryadapter_type0_fallback_len;
    int32 queryadapter_type0_fallback_ret;
    int32 queryadapter_type0_fallback_status;
    uint32 queryadapter_type0_result_route;
    uint32 queryadapter_type0_fallback_reason;
    uint32 queryadapter_type0_fallback_route;
    uint32 queryadapter_umd_rewrite_attempted;
    uint32 queryadapter_umd_rewrite_rewritten;
    uint32 queryadapter_umd_rewrite_path;
    uint32 queryadapter_umd_rewrite_vendor;
    uint32 queryadapter_umd_rewrite_original_hash;
    uint32 queryadapter_umd_rewrite_original0;
    uint32 queryadapter_umd_rewrite_original1;
    uint32 queryadapter_umd_rewrite_host_status;
    int32 queryadapter_umd_rewrite_ret;
    uint32 adapter_vendor_id;
    uint32 adapter_device_id;
    uint32 adapter_hardware_raw[7];
    uint32 adapter_hardware_raw_size;
    uint32 adapter_hardware_normalized;
    uint32 adapter_hardware_fallback;
    uint32 adapter_hardware_fallback_count;
    uint32 adapter_hardware_fallback_source;
    uint32 adapter_hardware_synthetic_rejected;
    uint32 adapter_hardware_v40_short_zero;
    uint32 adapter_hardware_v40_short_len;
    int32 adapter_hardware_v40_short_ret;
    int32 adapter_hardware_v40_short_status;
    uint32 adapter_hardware_cache_available;
    uint32 adapter_hardware_payload_len;
    uint32 adapter_hardware_temp_v27_attempts;
    uint32 adapter_hardware_temp_v27_successes;
    uint32 adapter_hardware_temp_v27_failures;
    int32 adapter_hardware_temp_v27_last_ret;
    int32 adapter_hardware_temp_v27_last_status;
    uint32 adapter_hardware_temp_v27_open_len;
    uint32 adapter_hardware_temp_v27_query_len;
    uint32 adapter_hardware_temp_v27_close_len;
    int32 adapter_hardware_temp_v27_close_ret;
    int32 adapter_hardware_temp_v27_close_status;
    uint32 adapter_hardware_temp_v27_handle;
    uint32 adapter_hardware_temp_v27_restored_version;
    uint32 adapter_hardware_temp_v27_restored_ext;
    uint32 enumadapters_last_cmd;
    uint32 enumadapters_last_in_count;
    uint32 enumadapters_last_out_count;
    uint32 enumadapters_last_num_sources;
    uint64 enumadapters_last_buffer;
    uint32 enumadapters_last_handle;
    uint32 enumadapters_last_luid_low;
    uint32 enumadapters_last_luid_high;
    uint32 enumadapters_last_luid_source;
    int32 enumadapters_last_ret;
    uint32 enumadapters_last_stage;
    int32 enumadapters_last_ensure_ret;
    int32 enumadapters_last_bind_ret;
    int32 enumadapters_last_local_ret;
    int32 enumadapters_last_copyout_ret;
    uint32 enumadapters_last_global_open;
    uint32 enumadapters_last_vgpu_open;
    uint32 enumadapters_last_global_relid;
    uint32 enumadapters_last_vgpu_relid;
    uint32 enumadapters_last_global_conn;
    uint32 enumadapters_last_vgpu_conn;
    uint32 enumadapters_last_host_adapter;
    uint32 enumadapters_last_probe_successes;
    int32 enumadapters_last_probe_ret;
    int32 enumadapters_last_probe_status;
    uint32 enumadapters_last_probe_handle;
    uint32 enumadapters_last_ready;
    uint32 enumadapters_last_process;
    uint32 enumadapters_last_process_created;
    uint32 enumadapters_last_process_generation;
    uint32 enumadapters_last_process_refs;
    uint32 enumadapters_last_process_adapters;
    uint32 enumadapters_last_process_locals;
    uint32 enumadapters_last_process_objects;
    uint32 openadapter_luid_last_input_low;
    uint32 openadapter_luid_last_input_high;
    uint32 openadapter_luid_last_um_low;
    uint32 openadapter_luid_last_um_high;
    uint32 openadapter_luid_last_host_low;
    uint32 openadapter_luid_last_host_high;
    uint32 openadapter_luid_last_host_basis;
    uint32 openadapter_luid_last_match;
    uint32 openadapter_luid_last_reject;
    uint32 openadapter_luid_last_handle;
    int32 openadapter_luid_last_ret;
    int32 openadapter_luid_last_status;
    uint32 local_adapter_namespace_hits;
    uint32 local_adapter_namespace_misses;
    uint32 local_adapter_last_result;
    uint32 local_adapter_last_handle;
    uint32 local_adapter_last_host;
    uint32 local_adapter_last_refs;
    uint32 local_adapter_last_locals;
    uint32 local_adapter_last_generation;
    uint32 local_adapter_reuse_delayed;
    uint32 local_adapter_reuse_allowed;
    uint32 local_adapter_min_free_entries;
    uint32 queryadapter_history_index;
    uint32 queryadapter_history_type[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_size[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_len[HV_DXG_QUERY_HISTORY_MAX];
    int32 queryadapter_history_ret[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_status[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_route[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_adapter[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_host_adapter[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_head_len[HV_DXG_QUERY_HISTORY_MAX];
    uint8 queryadapter_history_head[HV_DXG_QUERY_HISTORY_MAX]
                                  [HV_DXG_QUERYADAPTER_PAYLOAD_HEAD_BYTES];
    uint32 queryadapter_admission_history_index;
    uint32 queryadapter_admission_kind[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_type[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_size[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_len[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    int32 queryadapter_admission_ret[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_status[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_route[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_source[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_adapter[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_host[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_admission_head_hash[HV_DXG_QAI_ADMISSION_HISTORY_MAX];
    uint32 queryadapter_return_head_len;
    uint8 queryadapter_return_head[HV_DXG_QUERYADAPTER_PAYLOAD_HEAD_BYTES];
    struct hvdxg_queryadapter_alias_cache_entry queryadapter_alias_cache
        [HV_DXG_QUERYADAPTER_ALIAS_CACHE_TYPES];
    uint32 queryadapter_source_last;
    uint32 queryadapter_source_real_host;
    uint32 queryadapter_source_alias_cache;
    uint32 queryadapter_source_fallback;
    uint32 queryadapter_source_staged;
    uint32 queryadapter_alias_cache_hits;
    uint32 queryadapter_alias_cache_misses;
    uint32 queryadapter_alias_cache_stores;
    uint32 queryadapter_alias_cache_full;
    uint32 queryadapter_alias_cache_last_type;
    uint32 queryadapter_alias_cache_last_size;
    uint32 queryadapter_alias_cache_last_len;
    uint32 queryadapter_alias_cache_last_alias;
    uint32 queryadapter_alias_cache_last_host;
    uint32 queryadapter_alias_cache_last_hash;
    uint32 queryadapter_alias_cache_last_result;
    uint32 queryadapter_type0_cache_valid;
    uint32 queryadapter_type0_cache_size;
    uint32 queryadapter_type0_cache_host;
    uint32 queryadapter_type0_cache_input_hash;
    uint32 queryadapter_type0_cache_hash;
    uint8 *queryadapter_type0_cache_payload;
    uint32 queryadapter_alias_staged_type;
    uint32 queryadapter_alias_staged_size;
    uint32 queryadapter_alias_staged_alias;
    uint32 queryadapter_alias_staged_host;
    uint32 queryadapter_alias_staged_path;
    int32 queryadapter_alias_staged_ret;
    uint32 queryregistry_last_query_type;
    uint32 queryregistry_last_flags;
    uint32 queryregistry_last_value_type;
    uint32 queryregistry_last_phys;
    uint32 queryregistry_last_output_size;
    uint32 queryregistry_last_status;
    uint32 queryregistry_last_name0;
    uint32 queryregistry_last_name1;
    char queryregistry_last_name[64];
    uint32 feature_last_len;
    int32 feature_last_ret;
    int32 feature_last_status;
    uint32 feature_last_id;
    uint32 feature_last_result;
    uint32 schedprio_last_len;
    int32 schedprio_last_ret;
    int32 schedprio_last_status;
    int32 schedprio_last_value;
    uint32 schedprio_last_context;
    uint32 allocprio_last_len;
    int32 allocprio_last_ret;
    int32 allocprio_last_status;
    uint32 allocprio_last_count;
    uint32 allocres_last_len;
    int32 allocres_last_ret;
    int32 allocres_last_status;
    uint32 allocres_last_count;
    uint32 allocres_last_value;
    uint32 offer_last_len;
    int32 offer_last_ret;
    int32 offer_last_status;
    uint32 offer_last_count;
    uint32 statistics_last_len;
    int32 statistics_last_ret;
    int32 statistics_last_status;
    uint32 statistics_last_type;
    uint32 clockcalibration_last_len;
    int32 clockcalibration_last_ret;
    int32 clockcalibration_last_status;
    uint32 clockcalibration_last_node;
    uint64 clockcalibration_last_gpu_frequency;
    uint64 clockcalibration_last_gpu_counter;
    uint64 clockcalibration_last_cpu_counter;
    uint32 stdalloc_last_len;
    int32 stdalloc_last_ret;
    int32 stdalloc_last_status;
    uint32 stdalloc_last_alloc_size;
    uint32 stdalloc_last_res_size;
    uint32 escape_last_len;
    int32 escape_last_ret;
    uint32 escape_last_type;
    uint32 escape_last_flags;
    uint32 escape_last_size;
    uint32 shareobject_last_len;
    uint32 shareobject_last_cmd_len;
    uint32 shareobject_last_wire_len;
    uint32 shareobject_last_ext;
    uint32 shareobject_last_ext_offset;
    uint32 shareobject_last_device_offset;
    uint32 shareobject_last_object_offset;
    uint32 shareobject_last_result_len;
    uint32 shareobject_last_head_len;
    uint8 shareobject_last_head[HV_DXG_NTSHARED_RAW_BYTES];
    uint16 shareobject_last_completion_type;
    uint32 shareobject_last_completion_len;
    uint8 shareobject_last_completion_prefix[8];
    int32 shareobject_last_ret;
    int32 shareobject_last_status;
    uint32 shareobject_last_process;
    uint32 shareobject_last_device;
    uint32 shareobject_last_object;
    uint64 shareobject_last_reserved;
    uint64 shareobject_last_nt_handle;
    uint32 shareobject_diag_attempted;
    uint32 shareobject_diag_valid_nt;
    uint32 shareobject_diag_kind;
    uint32 shareobject_diag_reason;
    uint32 vgpu_send_last_command;
    uint32 vgpu_send_last_cmd_len;
    uint32 vgpu_send_last_wire_len;
    uint32 vgpu_send_last_ext;
    uint32 vgpu_send_last_ext_offset;
    uint32 vgpu_send_last_process;
    uint32 vgpu_send_last_channel;
    uint32 vgpu_send_last_route_global;
    uint32 vgpu_send_last_retries;
    int32 vgpu_send_last_ret;
    struct hvdxg_winluid vgpu_send_last_luid;
    uint32 global_send_last_command;
    uint32 global_send_last_cmd_len;
    uint32 global_send_last_wire_len;
    uint32 global_send_last_ext;
    uint32 global_send_last_ext_offset;
    uint32 global_send_last_process;
    uint32 global_send_last_channel;
    uint32 global_send_last_retries;
    int32 global_send_last_ret;
    struct hvdxg_winluid global_send_last_luid;
    struct hvdxg_global_send_diag global_send_ntshared;
    struct hvdxg_global_send_diag global_send_ntshared_ext;
    struct hvdxg_global_send_diag global_send_shareobject;
    struct hvdxg_global_send_diag global_send_destroynt;
    struct hvdxg_global_send_diag global_send_destroysync;
    uint32 ntshared_last_create_len;
    uint32 ntshared_last_create_cmd_len;
    uint32 ntshared_last_create_object_offset;
    uint32 ntshared_last_create_result_len;
    uint16 ntshared_last_create_completion_type;
    uint32 ntshared_last_create_completion_len;
    uint8 ntshared_last_create_completion_prefix[8];
    int32 ntshared_last_create_ret;
    uint32 ntshared_last_create_process;
    uint32 ntshared_last_create_type;
    uint32 ntshared_last_create_channel;
    uint32 ntshared_last_create_object;
    uint32 ntshared_last_create_handle;
    uint32 ntshared_last_create_raw0;
    uint32 ntshared_last_create_zero_len;
    uint32 ntshared_last_create_side_effect;
    uint32 ntshared_last_create_share_fallback;
    uint32 ntshared_last_create_share_valid;
    uint32 ntshared_create_attempts;
    uint32 ntshared_create_attempt_len[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_result_len[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint64 ntshared_create_attempt_cmdid[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_command[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_cmd_len[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_wire_len[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_ext[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_ext_offset[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_object_offset[HV_DXG_NTSHARED_ATTEMPT_MAX];
    int32 ntshared_create_attempt_ret[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_raw0[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_status[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_process[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_object[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_label[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_channel[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_monitor[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_monitorid[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_dedicated[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint32 ntshared_create_attempt_head_len[HV_DXG_NTSHARED_ATTEMPT_MAX];
    uint8 ntshared_create_attempt_head[HV_DXG_NTSHARED_ATTEMPT_MAX]
                                       [HV_DXG_NTSHARED_RAW_BYTES];
    uint32 ntshared_obj_found;
    uint32 ntshared_obj_exact;
    uint32 ntshared_obj_local;
    uint32 ntshared_obj_host;
    uint32 ntshared_obj_type;
    uint32 ntshared_obj_parent;
    uint32 ntshared_obj_device;
    uint32 ntshared_obj_owner_process;
    uint32 ntshared_obj_owner_generation;
    uint32 ntshared_obj_generation;
    uint32 ntshared_obj_stale;
    uint32 ntshared_obj_destroyed;
    uint32 ntshared_runtime_seen;
    uint32 ntshared_runtime_user_object;
    uint32 ntshared_runtime_user_device;
    uint32 ntshared_runtime_kind;
    uint32 ntshared_runtime_host_object;
    uint32 ntshared_runtime_host_device;
    uint32 ntshared_runtime_entry_found;
    uint32 ntshared_runtime_entry_exact;
    uint32 ntshared_runtime_entry_type;
    uint32 ntshared_runtime_entry_local;
    uint32 ntshared_runtime_entry_host;
    uint32 ntshared_runtime_entry_parent;
    uint32 ntshared_runtime_entry_device;
    uint32 ntshared_runtime_entry_generation;
    uint32 ntshared_runtime_entry_destroyed;
    uint32 ntshared_runtime_owner_process;
    uint32 ntshared_runtime_owner_generation;
    uint32 ntshared_runtime_owner_refs;
    uint32 ntshared_runtime_resource;
    uint32 ntshared_runtime_resource_host;
    uint32 ntshared_runtime_resource_flags;
    uint32 ntshared_runtime_alloc;
    uint32 ntshared_runtime_alloc_host;
    uint32 ntshared_runtime_alloc_flags;
    uint64 ntshared_runtime_alloc_size;
    uint32 ntshared_runtime_alloc_owner_process;
    uint32 ntshared_runtime_alloc_owner_generation;
    uint32 ntshared_runtime_alloc_owner_refs;
    uint32 ntshared_runtime_meta_created;
    uint32 ntshared_runtime_sealed_before;
    uint32 ntshared_runtime_sealed_after;
    uint32 ntshared_runtime_host_sealed_before;
    uint32 ntshared_runtime_host_sealed_after;
    uint32 ntshared_runtime_cmd_len;
    uint32 ntshared_runtime_wire_len;
    uint32 ntshared_runtime_ext;
    uint32 ntshared_runtime_ext_offset;
    uint32 ntshared_runtime_object_offset;
    uint32 ntshared_runtime_result_len;
    uint32 ntshared_runtime_return_len;
    int32 ntshared_runtime_return_ret;
    uint32 ntshared_runtime_return_raw;
    uint32 ntshared_runtime_return_handle;
    uint32 ntshared_last_destroy_len;
    int32 ntshared_last_destroy_ret;
    uint32 ntshared_last_destroy_status;
    uint32 ntshared_last_destroy_handle;
    struct hvdxg_ntshared_cache_entry ntshared_cache[HV_DXG_NTSHARED_CACHE_MAX];
    uint32 ntshared_cache_hits;
    uint32 ntshared_cache_misses;
    uint32 ntshared_cache_inserts;
    uint32 ntshared_cache_releases;
    uint32 ntshared_cache_destroys;
    uint32 ntshared_cache_full;
    uint32 ntshared_cache_last_kind;
    uint32 ntshared_cache_last_process;
    uint32 ntshared_cache_last_object;
    uint32 ntshared_cache_last_handle;
    uint32 ntshared_cache_last_refs;
    uint32 sharedhandle_last_cmd;
    int32 sharedhandle_last_ret;
    uint32 sharedhandle_last_device;
    uint32 sharedhandle_last_object;
    uint64 sharedhandle_last_nt_handle;
    uint32 sharedhandle_last_count;
    uint32 sharedhandle_last_global_share;
    uint32 sharedhandle_last_runtime_d3d12_flags;
    uint32 sharedhandle_last_kind;
    uint32 sharedhandle_last_fops_kind;
    uint32 sharedhandle_last_raw_device;
    uint32 sharedhandle_last_raw_object;
    uint32 sharedhandle_last_host_device;
    uint32 sharedhandle_last_host_device_found;
    uint32 sharedhandle_last_host_object;
    uint32 sharedhandle_last_object_found;
    uint32 sharedhandle_last_raw_resource_found;
    uint32 sharedhandle_last_raw_allocation_found;
    uint32 sharedhandle_last_raw_sync_found;
    uint64 sharedhandle_last_parent;
    uint32 sharedhandle_last_current_process;
    uint32 sharedhandle_last_current_generation;
    uint32 sharedhandle_last_creator_process;
    uint32 sharedhandle_last_creator_generation;
    uint32 sharedhandle_last_owner_process;
    uint32 sharedhandle_last_owner_generation;
    uint32 sharedhandle_last_owner_refs;
    uint32 sharedhandle_last_owner_used;
    uint32 sharedhandle_last_object_type;
    uint32 sharedhandle_last_object_device;
    uint32 sharedhandle_last_allocation;
    uint32 sharedhandle_last_allocation_found;
    uint32 sharedhandle_last_allocation_owner_process;
    uint32 sharedhandle_last_allocation_owner_generation;
    uint32 sharedhandle_last_allocation_owner_refs;
    uint64 sharedhandle_last_map_va;
    uint64 sharedhandle_last_map_pages;
    uint64 sharedhandle_last_map_fence;
    int32 sharedhandle_last_map_ret;
    uint32 sharedhandle_last_map_status;
    uint32 sharedhandle_last_resident_paging_queue;
    uint32 sharedhandle_last_resident_sync;
    uint64 sharedhandle_last_resident_fence;
    uint64 sharedhandle_last_resident_current;
    uint32 sharedhandle_last_resident_wait_result;
    int32 sharedhandle_last_resident_wait_ret;
    uint32 sharedhandle_last_resident_missing;
    uint32 sharedhandle_last_resident_enforced;
    uint32 sharedhandle_last_create_flags;
    uint32 sharedhandle_last_alloc_count;
    uint32 sharedhandle_last_sealed;
    uint32 sharedhandle_last_sync_type;
    uint32 sharedhandle_last_sync_flags;
    uint32 sharedhandle_last_sync_global;
    uint32 sharedhandle_last_sync_monitor_fence;
    uint64 sharedhandle_last_sync_fence_cpu;
    uint64 sharedhandle_last_sync_fence_kva;
    uint32 sharedsync_export_fd;
    int32 sharedsync_export_ret;
    uint32 sharedsync_export_cloexec;
    uint32 sharedsync_export_fops_kind;
    uint32 sharedsync_export_fd_kind;
    uint32 sharedsync_export_device;
    uint32 sharedsync_export_host_device;
    uint32 sharedsync_export_object;
    uint32 sharedsync_export_host_object;
    uint32 sharedsync_export_sync_type;
    uint32 sharedsync_export_sync_flags;
    uint32 sharedsync_export_monitor_fence;
    uint32 sharedsync_export_global_share;
    uint32 sharedsync_export_global_zero;
    uint32 sharedsync_export_host_shared_handle;
    uint32 sharedsync_export_host_nt_handle;
    uint32 sharedsync_export_nt_refs;
    uint32 sharedsync_export_shared_owner_object;
    uint32 sharedsync_export_cache_process;
    uint32 sharedsync_export_owner_process;
    uint32 sharedsync_export_owner_generation;
    uint32 sharedsync_export_owner_refs;
    uint32 shareobjects_last_desired_access;
    uint64 shareobjects_last_object_attr;
    uint32 shareobjects_last_attr_len;
    int32 shareobjects_last_attr_ret;
    uint8 shareobjects_last_attr_head[8];
    uint32 sharedresource_seals;
    uint32 sharedresource_seal_reuses;
    uint32 sharedresource_seal_denied;
    uint32 sharedresource_created;
    uint32 sharedresource_seal_allocs;
    uint32 sharedresource_seal_private;
    uint32 sharedresource_open_tracked;
    uint32 sharedresource_owner_exists;
    uint32 sharedresource_owner_cached;
    uint32 sharedresource_owner_reused;
    uint32 sharedresource_owner_nt;
    uint32 sharedresource_owner_refs;
    uint32 sharedresource_owner_sealed;
    uint32 sharedresource_owner_object;
    uint32 sharedresource_owner_process;
    uint32 sharedresource_open_global;
    uint32 sharedresource_pre_nt_sealable;
    int32 sharedresource_pre_nt_seal_ret;
    uint32 sharedresource_meta_track_host;
    uint32 sharedresource_meta_runtime_len;
    uint32 sharedresource_meta_resource_len;
    uint32 sharedresource_meta_total_len;
    uint32 sharedresource_meta_alloc0_priv;
    uint32 sharedresource_meta_runtime_hash;
    uint32 sharedresource_meta_resource_hash;
    uint32 sharedresource_meta_total_hash;
    uint32 sharedresource_meta_match_in;
    uint32 sharedresource_meta_match_out;
    uint32 sharedresource_meta_total_w4;
    uint32 sharedresource_meta_total_w8;
    uint32 sharedresource_meta_logical_flags;
    uint32 sharedresource_meta_host_result_flags;
    uint32 sharedresource_meta_host_flags_ignored;
    uint32 sharedresource_pre_nt_seal_applied;
    uint32 sharedresource_pre_nt_seal_before;
    uint32 sharedresource_pre_nt_seal_after;
    int32 sharedresource_pre_nt_seal_actual_ret;
    uint32 sharedresource_nt_resource_host;
    uint32 sharedresource_nt_alloc_host;
    uint32 sharedresource_nt_resource_flags;
    uint32 sharedresource_nt_alloc_flags;
    uint32 sharedresource_nt_alloc_in_hash;
    uint32 sharedresource_nt_alloc_out_hash;
    uint64 sharedresource_nt_alloc_size;
    uint32 sharedresource_nt_meta_before;
    uint32 sharedresource_nt_meta_after;
    uint32 sharedresource_nt_seal_before;
    uint32 sharedresource_nt_seal_after;
    uint32 sharedresource_nt_host_seal_before;
    uint32 sharedresource_nt_host_seal_after;
    uint32 sharedresource_seal_tracked_allocs;
    uint32 sharedresource_seal_expected_private;
    uint32 sharedresource_seal_actual_private;
    int32 sharedresource_seal_verify_ret;
    uint32 sharedresource_seal_missing_alloc;
    uint32 sharedresource_seal_extra_alloc;
    uint32 sharedresource_seal_append_rejects;
    uint32 sharedresource_seal_last_resource;
    uint32 sharedresource_seal_last_generation;
    uint32 sharedresource_record_valid;
    uint32 sharedresource_record_stage;
    uint32 sharedresource_record_key_kind;
    uint32 sharedresource_record_key_process;
    uint32 sharedresource_record_key_object;
    uint32 sharedresource_record_key_global;
    uint32 sharedresource_record_key_nt;
    uint32 sharedresource_record_source_process;
    uint64 sharedresource_record_source_tgid;
    uint32 sharedresource_record_source_generation;
    uint32 sharedresource_record_device;
    uint32 sharedresource_record_resource;
    uint32 sharedresource_record_allocation;
    uint32 sharedresource_record_adapter_low;
    uint32 sharedresource_record_adapter_high;
    uint32 sharedresource_record_host_adapter_low;
    uint32 sharedresource_record_host_adapter_high;
    uint32 sharedresource_record_sealed_generation;
    uint32 sharedresource_record_sealed;
    uint32 sharedresource_record_seal_before_fd;
    uint32 sharedresource_record_alloc_count;
    uint32 sharedresource_record_runtime_size;
    uint32 sharedresource_record_resource_size;
    uint32 sharedresource_record_total_size;
    uint32 sharedresource_record_alloc0_priv;
    uint32 sharedresource_record_runtime_hash;
    uint32 sharedresource_record_resource_hash;
    uint32 sharedresource_record_total_hash;
    uint32 sharedresource_record_alloc0_hash;
    uint32 sharedresource_record_nt_refs;
    uint32 sharedresource_record_query_count;
    uint32 sharedresource_record_open_count;
    uint32 sharedresource_record_fd_publish_count;
    int32 sharedresource_record_local_admit_ret;
    uint32 sharedresource_record_local_exact;
    uint32 sharedresource_record_mutation_after_seal;
    uint32 openresource_last_cmd_len;
    uint32 openresource_last_wire_len;
    uint32 openresource_last_ext;
    uint32 openresource_last_ext_offset;
    uint32 openresource_last_result_len;
    uint32 openresource_last_actual_len;
    int32 openresource_last_ret;
    int32 openresource_last_status;
    uint32 openresource_last_process;
    uint32 openresource_last_device;
    uint32 openresource_last_global;
    uint32 openresource_last_alloc_count;
    uint32 openresource_last_total_priv;
    uint32 openresource_last_result_resource;
    uint32 openresource_last_result_alloc0;
    uint32 openresource_last_seal_before;
    uint32 openresource_last_seal_after;
    uint32 openresource_last_fd_kind;
    uint32 openresource_last_fd_refs;
    uint32 openresource_last_route_global;
    uint32 openresource_last_fops_kind;
    uint32 queryresource_last_seen;
    int32 queryresource_last_ret;
    uint32 queryresource_last_device;
    uint64 queryresource_last_nt;
    uint32 queryresource_last_fd_kind;
    uint32 queryresource_last_fops_kind;
    uint32 queryresource_last_sync_probe;
    uint32 queryresource_last_global;
    uint32 queryresource_last_host_nt;
    uint32 queryresource_last_refs;
    uint32 queryresource_last_object;
    uint32 queryresource_last_cache_object;
    uint32 queryresource_last_alloc_count;
    uint32 queryresource_last_runtime_size;
    uint32 queryresource_last_resource_size;
    uint32 queryresource_last_total_size;
    uint32 opensync_last_cmd_len;
    uint32 opensync_last_wire_len;
    uint32 opensync_last_ext;
    uint32 opensync_last_ext_offset;
    uint32 opensync_last_result_len;
    uint32 opensync_last_actual_len;
    int32 opensync_last_ret;
    int32 opensync_last_status;
    uint32 opensync_last_process;
    uint32 opensync_last_device;
    uint32 opensync_last_device_host;
    uint32 opensync_last_device_owner;
    uint32 opensync_last_device_owner_generation;
    uint32 opensync_last_device_generation;
    uint32 opensync_last_global;
    uint64 opensync_last_input_nt;
    uint32 opensync_last_host_nt;
    uint32 opensync_last_object;
    uint32 opensync_last_cache_object;
    uint32 opensync_last_source_device;
    uint32 opensync_last_source_device_host;
    uint32 opensync_last_source_owner;
    uint32 opensync_last_source_owner_generation;
    uint32 opensync_last_source_flags;
    uint32 opensync_last_same_device;
    uint32 opensync_last_adapter_match;
    uint32 opensync_last_adapter_low;
    uint32 opensync_last_adapter_high;
    uint32 opensync_last_host_adapter_low;
    uint32 opensync_last_host_adapter_high;
    uint32 opensync_last_flags;
    uint32 opensync_last_wire_flags;
    uint32 opensync_last_forced_flags;
    uint32 opensync_last_fops_kind;
    uint32 opensync_last_sync_type;
    uint32 opensync_last_result_sync;
    uint64 opensync_last_gpu_va;
    uint64 opensync_last_cpu_pa;
    uint32 opensync_last_fd_kind;
    uint32 opensync_last_fd_refs;
    uint32 opensync_ioctl_count;
    uint32 opensync_last_gate;
    uint64 opensync_last_current_tgid;
    uint64 opensync_last_owner_tgid;
    uint32 opensync_last_owner_generation;
    uint32 opensync_last_namespace_mismatch;
    uint32 opensync_last_namespace_rejects;
    uint32 sharedclose_last_kind;
    uint32 sharedclose_last_process;
    uint32 sharedclose_last_object;
    uint32 sharedclose_last_nt_handle;
    uint32 sharedclose_last_global;
    uint32 sharedclose_last_refs_before;
    uint32 sharedclose_last_refs_after;
    uint32 sharedclose_last_fops_kind;
    uint32 sharedclose_last_cache_object;
    uint32 sharedclose_last_host_shared_handle;
    uint32 sharedclose_last_destroy_status;
    uint32 sharedclose_last_destroy_actual_len;
    uint32 sharedclose_last_destroy_handle_offset;
    uint32 sharedclose_last_destroy_handle;
    int32 sharedclose_last_destroy_ret;
    uint32 sharedclose_last_destroy_cmd_len;
    uint32 sharedclose_last_destroy_wire_len;
    uint32 sharedclose_last_destroy_ext;
    uint32 sharedclose_last_destroy_ext_offset;
    uint32 sharedclose_last_destroy_result_len;
    uint32 sharedclose_kind_seen[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_fops[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_refs_before[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_refs_after[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_handle[HV_DXG_SHARED_OBJECT_KIND_MAX];
    int32 sharedclose_kind_destroy_ret[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_status[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_actual_len[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_cmd_len[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_wire_len[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_ext[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 sharedclose_kind_destroy_result_len[HV_DXG_SHARED_OBJECT_KIND_MAX];
    uint32 ntshared_pre_resource_type;
    uint32 ntshared_pre_resource_local;
    uint32 ntshared_pre_resource_host;
    uint32 ntshared_pre_resource_generation;
    uint32 ntshared_pre_resource_destroyed;
    uint32 ntshared_pre_resource_refs;
    uint32 ntshared_pre_resource_index;
    uint32 ntshared_pre_resource_unique;
    uint32 ntshared_pre_resource_instance;
    uint32 ntshared_pre_resource_sealed;
    uint32 ntshared_pre_resource_open_count;
    uint32 ntshared_pre_alloc_type;
    uint32 ntshared_pre_alloc_local;
    uint32 ntshared_pre_alloc_host;
    uint32 ntshared_pre_alloc_generation;
    uint32 ntshared_pre_alloc_destroyed;
    uint32 ntshared_pre_alloc_refs;
    uint32 ntshared_pre_alloc_index;
    uint32 ntshared_pre_alloc_unique;
    uint32 ntshared_pre_alloc_instance;
    uint32 ntshared_pre_device_type;
    uint32 ntshared_pre_device_local;
    uint32 ntshared_pre_device_host;
    uint32 ntshared_pre_device_generation;
    uint32 ntshared_pre_device_destroyed;
    uint32 ntshared_pre_device_refs;
    uint32 ntshared_pre_device_index;
    uint32 ntshared_pre_device_unique;
    uint32 ntshared_pre_device_instance;
    uint32 ntshared_pre_shared_owner_found;
    uint32 ntshared_pre_shared_owner_type;
    uint32 ntshared_pre_shared_owner_local;
    uint32 ntshared_pre_shared_owner_host;
    uint32 ntshared_pre_shared_owner_generation;
    uint32 ntshared_pre_shared_owner_destroyed;
    uint32 ntshared_pre_shared_owner_index;
    uint32 ntshared_pre_shared_owner_unique;
    uint32 ntshared_pre_shared_owner_instance;
    uint32 ntshared_pre_shared_owner_object;
    uint32 ntshared_pre_shared_owner_process;
    uint32 ntshared_pre_shared_owner_refs;
    uint32 ntshared_pre_shared_owner_nt;
    uint32 ntshared_pre_shared_owner_sealed;
    uint32 ntshared_pre_runtime_size;
    uint32 ntshared_pre_runtime_hash;
    uint32 ntshared_pre_runtime_w[4];
    uint32 ntshared_pre_resource_priv_size;
    uint32 ntshared_pre_resource_priv_hash;
    uint32 ntshared_pre_resource_priv_w[4];
    uint32 ntshared_pre_total_priv_size;
    uint32 ntshared_pre_total_priv_hash;
    uint32 ntshared_pre_total_priv_w[4];
    uint32 ntshared_pre_alloc_out_size;
    uint32 ntshared_pre_alloc_out_hash;
    uint32 ntshared_pre_alloc_out_w[4];
    uint64 ntshared_pre_create_seq;
    uint64 ntshared_pre_first_nt_seq;
    uint64 ntshared_pre_event_seq;
    uint32 ntshared_model_process_state;
    uint32 ntshared_model_open_process;
    uint32 ntshared_model_open_generation;
    uint32 ntshared_model_open_refs;
    uint32 ntshared_model_global_process;
    uint32 ntshared_model_command_process;
    uint32 ntshared_model_resource_owner;
    uint32 ntshared_model_resource_generation;
    uint32 ntshared_model_alloc_owner;
    uint32 ntshared_model_alloc_generation;
    uint32 ntshared_model_cmd_eq_open;
    uint32 ntshared_model_cmd_eq_global;
    uint32 ntshared_model_cmd_eq_resource;
    uint32 ntshared_model_cmd_eq_alloc;
    uint32 ntshared_model_open_objects;
    uint32 ntshared_model_process_objects;
    uint32 ntshared_model_resources;
    uint32 ntshared_model_allocations;
    uint32 ntshared_model_devices;
    uint32 ntshared_model_contexts;
    uint32 ntshared_model_process_live;
    uint32 ntshared_model_process_generation;
    uint32 ntshared_cleanup_ret;
    uint32 ntshared_cleanup_cached_before;
    uint32 ntshared_cleanup_cached_after;
    uint32 ntshared_cleanup_refs_before;
    uint32 ntshared_cleanup_refs_after;
    uint32 ntshared_cleanup_object_before;
    uint32 ntshared_cleanup_object_after;
    uint32 ntshared_cleanup_nt_before;
    uint32 ntshared_cleanup_nt_after;
    uint32 ntshared_cleanup_sealed_before;
    uint32 ntshared_cleanup_sealed_after;
    uint32 ntshared_cleanup_cache_inserts_before;
    uint32 ntshared_cleanup_cache_inserts_after;
    uint32 unsupported_last_cmd;
    int32 unsupported_last_ret;
    uint32 unsupported_last_device;
    uint32 unsupported_last_handle;
    uint32 unsupported_last_count;
    uint32 unsupported_last_nr;
    uint32 unsupported_last_size;
    uint32 unsupported_last_name;
    uint32 markdevice_last_len;
    int32 markdevice_last_ret;
    int32 markdevice_last_status;
    uint32 markdevice_last_device;
    uint32 markdevice_last_reason;
    uint32 markdevice_last_process;
    uint32 markdevice_last_cmd_len;
    uint32 syncfile_last_cmd;
    int32 syncfile_last_ret;
    uint32 syncfile_last_device;
    uint32 syncfile_last_object;
    uint32 syncfile_last_context;
    uint64 syncfile_last_handle;
    uint64 syncfile_last_fence;
    uint32 syncfile_last_global;
    uint32 syncfile_last_host_nt;
    uint32 syncfile_last_source_flags;
    uint32 syncfile_last_open_flags;
    uint32 syncfile_last_len;
    int32 syncfile_last_status;
    uint32 syncfile_last_out_sync;
    uint64 syncfile_last_cpu_va;
    uint64 syncfile_last_gpu_va;
    uint32 updateallocproperty_last_len;
    int32 updateallocproperty_last_ret;
    int32 updateallocproperty_last_status;
    uint32 updateallocproperty_last_allocation;
    uint64 updateallocproperty_last_fence;
    uint32 vidmem_reservation_last_len;
    int32 vidmem_reservation_last_ret;
    int32 vidmem_reservation_last_status;
    uint32 vidmem_reservation_last_group;
    uint64 vidmem_reservation_last_value;
    uint32 reclaim_last_len;
    int32 reclaim_last_ret;
    int32 reclaim_last_status;
    uint32 reclaim_last_count;
    uint32 reclaim_last_result0;
    uint64 reclaim_last_fence;
    uint32 updategpuva_last_len;
    int32 updategpuva_last_ret;
    int32 updategpuva_last_status;
    uint32 updategpuva_last_ops;
    uint64 updategpuva_last_fence;
    uint32 updategpuva_last_device;
    uint32 updategpuva_last_context;
    uint32 updategpuva_last_fence_object;
    uint32 updategpuva_last_flags;
    uint32 updategpuva_last_cmd_len;
    uint32 updategpuva_last_op_offset;
    uint32 updategpuva_last_op_size;
    uint32 updategpuva_last_op0_type;
    uint64 updategpuva_last_op0_base;
    uint64 updategpuva_last_op0_size;
    uint32 updategpuva_last_op0_allocation;
    uint64 updategpuva_last_op0_alloc_offset;
    uint64 updategpuva_last_op0_alloc_size;
    uint64 updategpuva_last_op0_source;
    uint64 updategpuva_last_op0_dest;
    uint64 updategpuva_last_op0_protection;
    uint64 updategpuva_last_op0_driver_protection;
    uint32 cacheops_last_len;
    int32 cacheops_last_ret;
    int32 cacheops_last_status;
    uint32 cacheops_last_allocation;
    uint32 submit_last_len;
    int32 submit_last_ret;
    int32 submit_last_status;
    uint64 submit_last_command_buffer;
    uint32 submit_last_command_length;
    uint32 submit_last_flags;
    uint32 submit_last_priv_size;
    uint32 submit_last_context_count;
    uint32 submit_last_context0;
    uint32 lock2_last_len;
    int32 lock2_last_ret;
    int32 lock2_last_status;
    uint32 lock2_last_allocation;
    uint64 lock2_last_offset;
    uint64 lock2_last_user_va;
    uint32 lock2_ioctl_count;
    uint32 lock2_host_forward_count;
    uint32 lock2_cached_ref_count;
    uint32 lock2_sysmem_count;
    uint32 lock2_map_fail_count;
    uint32 lock2_diag_prints;
    uint32 lock2_history_index;
    uint32 lock2_history_len[HV_DXG_RESOURCE_HISTORY_MAX];
    int32 lock2_history_ret[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 lock2_history_status[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 lock2_history_device[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 lock2_history_allocation[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 lock2_history_offset[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 lock2_history_user_va[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 lock2_history_map_size[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 unlock2_last_len;
    int32 unlock2_last_ret;
    int32 unlock2_last_status;
    uint32 unlock2_last_allocation;
    uint32 unlock2_ioctl_count;
    uint32 unlock2_host_forward_count;
    uint32 unlock2_missing_tracking_count;
    uint32 unlock2_cached_ref_count;
    uint32 unlock2_diag_prints;
    uint32 createcontext_last_len;
    int32 createcontext_last_ret;
    uint32 createcontext_last_handle;
    uint32 createcontext_last_device;
    uint32 createcontext_last_node;
    uint32 createcontext_last_engine;
    uint32 createcontext_last_flags;
    uint32 createcontext_last_hint;
    uint32 createcontext_last_priv_size;
    uint32 createcontext_last_priv_head_len;
    uint8 createcontext_last_priv_head[HV_DXG_CONTEXT_PRIV_HEAD_MAX];
    uint32 createcontext_fail_len;
    int32 createcontext_fail_ret;
    int32 createcontext_fail_status;
    uint32 flushdevice_last_len;
    int32 flushdevice_last_ret;
    int32 flushdevice_last_status;
    uint32 flushdevice_last_device;
    uint32 flushdevice_last_reason;
    uint32 createhwqueue_last_len;
    int32 createhwqueue_last_ret;
    int32 createhwqueue_last_status;
    uint32 createhwqueue_last_context;
    uint32 createhwqueue_last_flags;
    uint32 createhwqueue_last_priv_size;
    uint32 createhwqueue_last_priv_head_len;
    uint8 createhwqueue_last_priv_head[HV_DXG_HWQUEUE_PRIV_HEAD_MAX];
    uint32 createhwqueue_last_queue;
    uint32 createhwqueue_last_fence;
    uint64 createhwqueue_last_fence_cpu;
    uint64 createhwqueue_last_fence_gpu;
    uint32 submithwqueue_last_len;
    int32 submithwqueue_last_ret;
    uint32 submithwqueue_last_queue;
    uint64 submithwqueue_last_fence_id;
    uint32 submithwqueue_last_command_length;
    uint32 submithwqueue_last_priv_size;
    uint32 submithwqueue_last_priv_head_len;
    uint8 submithwqueue_last_priv_head[HV_DXG_HWQUEUE_SUBMIT_PRIV_HEAD_MAX];
    uint32 submithwqueue_history_index;
    uint32 submithwqueue_history_len[HV_DXG_RESOURCE_HISTORY_MAX];
    int32 submithwqueue_history_ret[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 submithwqueue_history_queue[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 submithwqueue_history_command_length[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 submithwqueue_history_priv_size[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 submithwqueue_history_priv_head_len[HV_DXG_RESOURCE_HISTORY_MAX];
    uint8 submithwqueue_history_priv_head[HV_DXG_RESOURCE_HISTORY_MAX][8];
    uint32 destroyhwqueue_last_len;
    int32 destroyhwqueue_last_ret;
    struct hvdxg_d3dkmthandle dxg_process;
    uint32 dxg_process_created;
    uint32 host_adapter_handle;
    struct hvdxg_winluid adapter_luid;
    struct hvdxg_winluid host_adapter_luid;
    struct hvdxg_winluid host_vgpu_luid;
    uint32 use_ext_header;
    uint32 active_vmbus_version;
    uint32 active_vmbus_last_compat;
    uint32 active_vmbus_source;
    uint32 active_vmbus_fallbacks;
    uint32 pci_domain;
    uint32 pci_bus;
    uint32 pci_dev;
    uint32 pci_func;
    uint32 pci_vendor;
    uint32 pci_dxg_device;
    uint32 pci_class;
    uint32 pci_dxg_vmbus_version;
    uint32 pci_dxg_vmbus_negotiated_version;
    uint32 pci_dxg_vmbus_write_attempted;
    uint32 pci_dxg_vmbus_writes;
    uint32 pci_dxg_vmbus_write_value;
    uint32 pci_dxg_vmbus_write_readback;
    int32 pci_dxg_vmbus_write_ret;
    uint32 pci_dxg_vmbus_write_config_supported;
    int32 pci_dxg_vmbus_write_config_ret;
    int32 pci_dxg_vmbus_write_verify_ret;
    uint32 pci_dxg_guid[4];
    struct hvdxg_winluid pci_host_vgpu_luid;
    uint32 pci_guestcaps_attempts;
    uint32 pci_guestcaps_writes;
    uint32 pci_guestcaps_found;
    uint32 pci_guestcaps_scan_done;
    uint32 pci_guestcaps_write_attempted;
    uint32 pci_guestcaps_write_verified;
    uint32 pci_guestcaps_offset;
    uint32 pci_guestcaps_value;
    uint32 pci_guestcaps_readback;
    int32 pci_guestcaps_ret;
    uint32 pci_guestcaps_before_probe;
    uint32 pci_guestcaps_busdevfn;
    uint32 pci_guestcaps_source;
    uint32 pci_guestcaps_token;
    uint16 probe_last_type;
    uint32 probe_last_len;
    uint8 probe_last_prefix[HV_DXG_PREFIX_BYTES];
    uint64 global_ring_pa;
    uint64 vgpu_ring_pa;
    uint8 *global_ring;
    uint8 *vgpu_ring;
    struct hv_ring_buffer *global_out_ring;
    struct hv_ring_buffer *global_in_ring;
    struct hv_ring_buffer *vgpu_out_ring;
    struct hv_ring_buffer *vgpu_in_ring;
    struct hv_guid global_instance;
    struct hv_guid vgpu_instance;
} hvdxg;

static cdev_t hvdxg_cdev;

static void hvdxg_process_channel_packets(struct hv_ring_buffer *in_ring,
                                          uint32 source_channel,
                                          uint32 source_relid,
                                          uint32 *counter);
static void hvdxg_pump_channels(void);
static void hvdxg_command_vgpu_init(struct hvdxg_command_vgpu_to_host *hdr,
                                    uint32 command_type);
static void hvdxg_command_vgpu_init_process(
    struct hvdxg_command_vgpu_to_host *hdr, uint32 command_type,
    struct hvdxg_d3dkmthandle process);
static inline void hvdxg_wc_store_fence(void);
static void hvdxg_command_vm_init(struct hvdxg_command_vm_to_host *hdr,
                                  uint32 command_type);
static int hvdxg_d3dkmt_ensure_adapter(void);
static int hvdxg_d3dkmt_ensure(void);
static int hvdxg_destroy_process_host(struct hvdxg_d3dkmthandle process);
static int hvdxg_luid_equal(struct hvdxg_winluid a, struct hvdxg_winluid b);
static int hvdxg_luid_nonzero(struct hvdxg_winluid luid);
static struct hvdxg_winluid hvdxg_user_adapter_luid(uint32 *source_out);
static struct hvdxg_winluid hvdxg_ext_adapter_luid(
    const struct hvdxg_winluid *process_luid);
static uint32 hvdxg_clamp_vmbus_version(uint32 version);
static void hvdxg_set_active_vmbus_version(uint32 version, uint32 source,
                                           uint32 last_compat);
static int hvdxg_write_pci_vmbus_version(uint32 version);
static struct hvdxg_winluid hvdxg_luid_from_guid(const struct hv_guid *guid);
static void hvdxg_capture_queryadapter_completion(void);
static int hvdxg_ntstatus_plausible(struct hvdxg_ntstatus status);
static int hvdxg_ntstatus_to_errno(struct hvdxg_ntstatus status);
static uint32 hvdxg_sync_type_is_monitored(uint32 type);
static int hvdxg_send_sync_vgpu(const void *cmd, uint32 cmd_len,
                                void *result, uint32 result_len,
                                uint32 *actual_len);
static int hvdxg_send_sync_vgpu_flags(const void *cmd, uint32 cmd_len,
                                      void *result, uint32 result_len,
                                      uint32 *actual_len, uint32 flags);
static int hvdxg_send_sync_vgpu_flags_luid(
    const void *cmd, uint32 cmd_len, void *result, uint32 result_len,
    uint32 *actual_len, uint32 flags,
    const struct hvdxg_winluid *ext_luid);
static int hvdxg_send_sync_global(const void *cmd, uint32 cmd_len,
                                  void *result, uint32 result_len,
                                  uint32 *actual_len);
static int hvdxg_send_sync_global_ex(const void *cmd, uint32 cmd_len,
                                     void *result, uint32 result_len,
                                     uint32 *actual_len,
                                     int force_ext_header,
                                     int ext_host_vgpu_luid,
                                     int suppress_ext_header);
static int hvdxg_probe_transport(void);
static uint64 hvdxg_alloc_host_event(void);
static uint64 hvdxg_alloc_host_event_file(struct vfs_file *file,
                                          int remove_after_signal);
static void hvdxg_remove_host_event(uint64 id);
static void hvdxg_pump_events_ms(uint64 timeout_ms);
static void hvdxg_note_cpu_wait_state(
    struct hvdxg_open_state *owner,
    const struct hvdxg_d3dkmthandle *objects,
    const uint64 *fence_values, uint32 object_count, uint64 event_id,
    uint32 async_event, uint32 result);
static int hvdxg_wait_host_event_or_cpu_fence(
    struct hvdxg_open_state *owner,
    const struct hvdxg_d3dkmthandle *objects,
    const uint64 *fence_values, uint32 object_count, int wait_any,
    uint64 event_id, uint64 timeout_ms);
static int hvdxg_send_waitsyncobjectfromcpu(
    struct d3dkmt_waitforsynchronizationobjectfromcpu *req,
    const void *objects, const void *fence_values, uint64 event_id,
    uint32 object_size, uint32 fence_size, uint32 *actual_len);

static uint32 hvdxg_parse_hex32(const char *s, const char **end)
{
    uint32 value = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s != '\0') {
        uint32 digit;

        if (*s >= '0' && *s <= '9')
            digit = (uint32)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            digit = (uint32)(*s - 'a') + 10;
        else if (*s >= 'A' && *s <= 'F')
            digit = (uint32)(*s - 'A') + 10;
        else
            break;
        value = (value << 4) | digit;
        s++;
    }
    if (end != NULL)
        *end = s;
    return value;
}

static void hvdxg_apply_cmdline_host_luid(void)
{
    char value[32];
    const char *p;
    uint32 high;
    uint32 low;

    if (cmdline_get_param("dxg_host_vgpu_luid", value, sizeof(value)) != 0)
        return;
    p = value;
    high = hvdxg_parse_hex32(p, &p);
    low = 0;
    if (*p == ':' || *p == '-')
        low = hvdxg_parse_hex32(p + 1, NULL);
    else
        low = high;
    hvdxg.host_vgpu_luid.a = low;
    hvdxg.host_vgpu_luid.b = high;
}

static void hvdxg_note_missing_pci_guestcaps_once(void);
static const char *hvdxg_pci_guestcaps_source_name(void);

static uint32 hvdxg_clamp_vmbus_version(uint32 version)
{
    return version >= HV_DXG_VMBUS_INTERFACE_VERSION ?
           HV_DXG_VMBUS_INTERFACE_VERSION :
           HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
}

static int hvdxg_host_v40_signal(void)
{
    return hvdxg.pci_dxg_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION;
}

static void hvdxg_set_active_vmbus_version(uint32 version, uint32 source,
                                           uint32 last_compat)
{
    hvdxg.active_vmbus_version = version;
    hvdxg.active_vmbus_source = source;
    hvdxg.active_vmbus_last_compat = last_compat;
    hvdxg.use_ext_header =
        version >= HV_DXG_VMBUS_INTERFACE_VERSION ? 1 : 0;
}

static int hvdxg_write_pci_vmbus_version(uint32 version)
{
    int ret = -ENODEV;
    uint32 readback = 0;

    hvdxg.pci_dxg_vmbus_write_attempted = 1;
    hvdxg.pci_dxg_vmbus_write_value = version;
    hvdxg.pci_dxg_vmbus_write_readback = 0;
    hvdxg.pci_dxg_vmbus_write_config_supported = 0;
    hvdxg.pci_dxg_vmbus_write_config_ret = -ENODEV;
    hvdxg.pci_dxg_vmbus_write_verify_ret = 0;

    if (hvdxg.pci_guestcaps_source == 2) {
        hvdxg.pci_dxg_vmbus_write_config_supported =
            hvpci.backend_registered && hvpci.config_window_ok ? 1 : 0;
        ret = hvpci_config_write(&hvpci, hvdxg.pci_guestcaps_token,
                                 HV_DXG_PCI_VMBUS_VERSION_OFFSET, 4,
                                 version);
        hvdxg.pci_dxg_vmbus_write_config_ret = ret;
        if (ret == 0) {
            hvdxg.pci_dxg_vmbus_writes++;
            hvdxg.pci_dxg_vmbus_write_verify_ret =
                hvpci_config_read(&hvpci, hvdxg.pci_guestcaps_token,
                                  HV_DXG_PCI_VMBUS_VERSION_OFFSET, 4,
                                  &readback);
        }
    } else if (hvdxg.pci_guestcaps_source == 1) {
        hvdxg.pci_dxg_vmbus_write_config_supported = 1;
        ret = pci_config_try_write32((uint8)hvdxg.pci_bus,
                                     (uint8)hvdxg.pci_dev,
                                     (uint8)hvdxg.pci_func,
                                     HV_DXG_PCI_VMBUS_VERSION_OFFSET,
                                     version);
        hvdxg.pci_dxg_vmbus_write_config_ret = ret;
        if (ret == 0) {
            hvdxg.pci_dxg_vmbus_writes++;
            readback = pci_config_read32((uint8)hvdxg.pci_bus,
                                         (uint8)hvdxg.pci_dev,
                                         (uint8)hvdxg.pci_func,
                                         HV_DXG_PCI_VMBUS_VERSION_OFFSET);
        }
    }

    hvdxg.pci_dxg_vmbus_write_readback = readback;
    hvdxg.pci_dxg_vmbus_write_ret = ret;
    return hvdxg.pci_dxg_vmbus_write_ret;
}

static void hvdxg_note_pci_vmbus_version(uint32 source)
{
    uint32 negotiated;

    hvdxg.pci_guestcaps_source = source;
    negotiated = hvdxg_clamp_vmbus_version(hvdxg.pci_dxg_vmbus_version);
    hvdxg.pci_dxg_vmbus_negotiated_version = negotiated;
    if (hvdxg.pci_dxg_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION)
        (void)hvdxg_write_pci_vmbus_version(negotiated);
    hvdxg_set_active_vmbus_version(negotiated, source,
                                   HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
}

void hyperv_dxg_note_pci(uint32 domain, uint32 bus, uint32 dev, uint32 func,
                         uint32 vendor, uint32 device, uint32 class_code,
                         uint32 guid0, uint32 guid1, uint32 guid2,
                         uint32 guid3, uint32 vmbus_version,
                         uint32 luid_low, uint32 luid_high,
                         uint32 guestcaps_offset, uint32 guestcaps_value,
                         uint32 guestcaps_readback, int guestcaps_ret)
{
    hvdxg.pci_domain = domain;
    hvdxg.pci_bus = bus;
    hvdxg.pci_dev = dev;
    hvdxg.pci_func = func;
    hvdxg.pci_vendor = vendor;
    hvdxg.pci_dxg_device = device;
    hvdxg.pci_class = class_code;
    hvdxg.pci_dxg_vmbus_version = vmbus_version;
    hvdxg.pci_dxg_guid[0] = guid0;
    hvdxg.pci_dxg_guid[1] = guid1;
    hvdxg.pci_dxg_guid[2] = guid2;
    hvdxg.pci_dxg_guid[3] = guid3;
    hvdxg.pci_host_vgpu_luid.a = luid_low;
    hvdxg.pci_host_vgpu_luid.b = luid_high;
    if (luid_low != 0 || luid_high != 0)
        hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
    hvdxg.pci_guestcaps_found = 1;
    hvdxg.pci_guestcaps_scan_done = 1;
    hvdxg.pci_guestcaps_write_attempted = 1;
    hvdxg.pci_guestcaps_write_verified =
        guestcaps_readback == guestcaps_value ? 1 : 0;
    hvdxg.pci_guestcaps_attempts = 1;
    hvdxg.pci_guestcaps_writes = 1;
    hvdxg.pci_guestcaps_offset = guestcaps_offset;
    hvdxg.pci_guestcaps_value = guestcaps_value;
    hvdxg.pci_guestcaps_readback = guestcaps_readback;
    hvdxg.pci_guestcaps_ret =
        guestcaps_ret == 0 && guestcaps_readback != guestcaps_value ?
        -EIO : guestcaps_ret;
    hvdxg.pci_guestcaps_before_probe =
        hvdxg.probe_attempts == 0 && !hvdxg.d3dkmt_ready ? 1 : 0;
    hvdxg.pci_guestcaps_busdevfn = (bus << 16) | (dev << 8) | func;
    hvdxg.pci_guestcaps_token = hvdxg.pci_guestcaps_busdevfn;
    hvdxg_note_pci_vmbus_version(1);
}

static int hvdxg_hvpci_child_is_dxg(uint32 i)
{
    uint32 class_code;

    if (i >= hvpci.child_count || i >= HVPCI_CHILD_MAX)
        return 0;
    if (!hvpci.child[i].registered)
        return 0;
    if (hvpci.child[i].vendor_id != PCI_VENDOR_MICROSOFT)
        return 0;
    class_code = hvpci.child[i].class_code;
    if (hvpci.child[i].device_id == PCI_DEVICE_MS_VIRTUAL_RENDER ||
        hvpci.child[i].device_id == PCI_DEVICE_MS_COMPUTE_ACCELERATOR)
        return 1;
    return (class_code >> 16) == 0x03;
}

static int hvdxg_try_hvpci_guestcaps(void)
{
    uint32 readback = 0;
    uint32 class_rev = 0;
    uint32 guid0 = 0;
    uint32 guid1 = 0;
    uint32 guid2 = 0;
    uint32 guid3 = 0;
    uint32 vmbus_version = 0;
    uint32 luid0 = 0;
    uint32 luid1 = 0;
    uint32 token;
    uint32 bus;
    uint32 dev;
    uint32 func;
    struct hvdxg_winluid pci_luid;
    int luid_equiv;
    int ret;

    if (!hvpci.backend_registered || !hvpci.config_window_ok)
        return -ENODEV;

    for (uint32 i = 0; i < hvpci.child_count && i < HVPCI_CHILD_MAX; i++) {
        if (!hvdxg_hvpci_child_is_dxg(i))
            continue;

        token = hvpci.child[i].win_slot;
        bus = hvpci.child[i].bus;
        dev = hvpci.child[i].dev;
        func = hvpci.child[i].func;

        hvdxg.pci_guestcaps_attempts++;
        hvdxg.pci_guestcaps_found = 1;
        hvdxg.pci_guestcaps_scan_done = 1;
        hvdxg.pci_guestcaps_write_attempted = 1;
        hvdxg.pci_guestcaps_token = token;
        hvdxg.pci_guestcaps_offset = HV_DXG_PCI_GUESTCAPS_OFFSET;
        hvdxg.pci_guestcaps_value = HV_DXG_PCI_GUESTCAPS_WSL2;
        hvdxg.pci_guestcaps_before_probe =
            hvdxg.probe_attempts == 0 && !hvdxg.d3dkmt_ready ? 1 : 0;
        hvdxg.pci_guestcaps_busdevfn = (bus << 16) | (dev << 8) | func;

        ret = hvpci_config_write(&hvpci, token,
                                 HV_DXG_PCI_GUESTCAPS_OFFSET, 4,
                                 HV_DXG_PCI_GUESTCAPS_WSL2);
        if (ret == 0) {
            hvdxg.pci_guestcaps_writes++;
            ret = hvpci_config_read(&hvpci, token,
                                    HV_DXG_PCI_GUESTCAPS_OFFSET, 4,
                                    &readback);
        }
        hvdxg.pci_guestcaps_readback = readback;

        (void)hvpci_config_read(&hvpci, token, 0x08, 4, &class_rev);
        (void)hvpci_config_read(&hvpci, token, 192, 4, &guid0);
        (void)hvpci_config_read(&hvpci, token, 196, 4, &guid1);
        (void)hvpci_config_read(&hvpci, token, 200, 4, &guid2);
        (void)hvpci_config_read(&hvpci, token, 204, 4, &guid3);
        (void)hvpci_config_read(&hvpci, token, 208, 4, &vmbus_version);
        (void)hvpci_config_read(&hvpci, token, 212, 4, &luid0);
        (void)hvpci_config_read(&hvpci, token, 216, 4, &luid1);
        pci_luid.a = luid0;
        pci_luid.b = luid1;
        /*
         * On the Hyper-V vPCI path, offset 212 is write-only guest caps from
         * the guest's perspective but reads back as the host vGPU LUID low
         * dword. Treat the guestcaps write as accepted when the config write
         * succeeded and the PCI LUID is valid/equivalent; keep raw readback
         * visible for diagnostics.
         */
        luid_equiv = hvdxg_luid_nonzero(pci_luid) &&
            (!hvdxg_luid_nonzero(hvdxg.host_vgpu_luid) ||
             hvdxg_luid_equal(pci_luid, hvdxg.host_vgpu_luid));
        hvdxg.pci_guestcaps_write_verified =
            ret == 0 && luid_equiv ? 1 : 0;
        hvdxg.pci_guestcaps_ret =
            ret == 0 && !luid_equiv ? -EIO : ret;
        hvdxg.pci_domain = 0;
        hvdxg.pci_bus = bus;
        hvdxg.pci_dev = dev;
        hvdxg.pci_func = func;
        hvdxg.pci_vendor = hvpci.child[i].vendor_id;
        hvdxg.pci_dxg_device = hvpci.child[i].device_id;
        hvdxg.pci_class = class_rev != 0xffffffffU && class_rev != 0 ?
                          class_rev >> 8 : hvpci.child[i].class_code;
        hvdxg.pci_dxg_vmbus_version = vmbus_version;
        hvdxg.pci_dxg_guid[0] = guid0;
        hvdxg.pci_dxg_guid[1] = guid1;
        hvdxg.pci_dxg_guid[2] = guid2;
        hvdxg.pci_dxg_guid[3] = guid3;
        hvdxg.pci_host_vgpu_luid = pci_luid;
        if (hvdxg_luid_nonzero(pci_luid))
            hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
        hvdxg_note_pci_vmbus_version(2);
        return hvdxg.pci_guestcaps_ret;
    }
    return -ENODEV;
}

static int hvdxg_try_pci_guestcaps_scan(void)
{
    int ret;

    if (hvdxg.pci_guestcaps_write_verified)
        return 0;
    if (hvdxg.pci_guestcaps_scan_done)
        return hvdxg.pci_guestcaps_ret;

    ret = hvdxg_try_hvpci_guestcaps();
    if (ret != -ENODEV)
        return ret;

    hvdxg.pci_guestcaps_scan_done = 1;
    for (uint32 bus = 0; bus < 256; bus++) {
        for (uint32 dev = 0; dev < 32; dev++) {
            for (uint32 func = 0; func < 8; func++) {
                uint32 id = pci_config_read32((uint8)bus, (uint8)dev,
                                              (uint8)func, 0);
                uint32 vendor = id & 0xffffU;
                uint32 device = (id >> 16) & 0xffffU;
                uint32 class_rev;
                uint32 class_code;
                uint32 guid0;
                uint32 guid1;
                uint32 guid2;
                uint32 guid3;
                uint32 vmbus_version;
                uint32 luid0;
                uint32 luid1;
                uint32 readback;
                uint16 command;
                int write_ret;

                if (vendor == 0xffffU || vendor == 0)
                    continue;
                if (vendor != PCI_VENDOR_MICROSOFT ||
                    (device != PCI_DEVICE_MS_VIRTUAL_RENDER &&
                     device != PCI_DEVICE_MS_COMPUTE_ACCELERATOR))
                    continue;

                class_rev = pci_config_read32((uint8)bus, (uint8)dev,
                                              (uint8)func, 0x08);
                class_code = class_rev >> 8;
                guid0 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 192);
                guid1 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 196);
                guid2 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 200);
                guid3 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 204);
                command = pci_config_read16((uint8)bus, (uint8)dev,
                                            (uint8)func, 0x04);
                command |= PCIE_CSCMD_MAE | PCIE_CSCMD_BME;
                pci_config_write16((uint8)bus, (uint8)dev, (uint8)func,
                                   0x04, command);
                hvdxg.pci_guestcaps_attempts++;
                hvdxg.pci_guestcaps_write_attempted = 1;
                hvdxg.pci_guestcaps_source = 1;
                pci_config_write32((uint8)bus, (uint8)dev, (uint8)func,
                                   HV_DXG_PCI_GUESTCAPS_OFFSET,
                                   HV_DXG_PCI_GUESTCAPS_WSL2);
                readback = pci_config_read32((uint8)bus, (uint8)dev,
                                             (uint8)func,
                                             HV_DXG_PCI_GUESTCAPS_OFFSET);
                write_ret = readback == HV_DXG_PCI_GUESTCAPS_WSL2 ?
                            0 : -EIO;
                vmbus_version = pci_config_read32((uint8)bus, (uint8)dev,
                                                  (uint8)func, 208);
                luid0 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 212);
                luid1 = pci_config_read32((uint8)bus, (uint8)dev,
                                          (uint8)func, 216);
                hyperv_dxg_note_pci(0, bus, dev, func, vendor, device,
                                    class_code, guid0, guid1, guid2, guid3,
                                    vmbus_version, luid0, luid1,
                                    HV_DXG_PCI_GUESTCAPS_OFFSET,
                                    HV_DXG_PCI_GUESTCAPS_WSL2, readback,
                                    write_ret);
                return write_ret;
            }
        }
    }

    return -ENODEV;
}

static void hvdxg_note_missing_pci_guestcaps_once(void)
{
    if (hvdxg.pci_guestcaps_attempts != 0)
        return;

    hvdxg.pci_guestcaps_attempts++;
    hvdxg.pci_guestcaps_scan_done = 1;
    hvdxg.pci_guestcaps_found = 0;
    hvdxg.pci_guestcaps_write_attempted = 0;
    hvdxg.pci_guestcaps_write_verified = 0;
    hvdxg.pci_guestcaps_offset = HV_DXG_PCI_GUESTCAPS_OFFSET;
    hvdxg.pci_guestcaps_value = HV_DXG_PCI_GUESTCAPS_WSL2;
    hvdxg.pci_guestcaps_readback = 0;
    hvdxg.pci_guestcaps_ret = -ENODEV;
    hvdxg.pci_guestcaps_before_probe =
        hvdxg.probe_attempts == 0 && !hvdxg.d3dkmt_ready ? 1 : 0;
    hvdxg.pci_guestcaps_busdevfn = 0;
    hvdxg.pci_guestcaps_source = 0;
    hvdxg.pci_guestcaps_token = 0;
    hvdxg.pci_dxg_vmbus_negotiated_version =
        HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
    hvdxg_set_active_vmbus_version(HV_DXG_VMBUS_INTERFACE_VERSION_OLD, 0,
                                   HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);
}

static const char *hvdxg_pci_guestcaps_source_name(void)
{
    switch (hvdxg.pci_guestcaps_source) {
    case 1:
        return "legacy-cf8";
    case 2:
        return "hyperv-vpci";
    default:
        return "none/legacy-scan";
    }
}

static void hvdxg_write_utf16_string(uint8 *dst, uint32 dst_size,
                                     const char *string)
{
    uint16 *out = (uint16 *)dst;
    uint32 max_chars = dst_size / sizeof(uint16);
    uint32 i = 0;

    if (max_chars == 0)
        return;
    while (i + 1 < max_chars && string[i] != '\0') {
        out[i] = (uint16)string[i];
        i++;
    }
    out[i] = 0;
}

static int hvdxg_queryadapter_admission_type(uint32 type);
static void hvdxg_note_queryadapter_admission(uint32 kind, uint32 type,
                                              uint32 size, uint32 out_len,
                                              int32 ret, uint32 status,
                                              uint32 route, uint32 source,
                                              uint32 adapter,
                                              uint32 host_adapter,
                                              uint32 head_hash);

static void hvdxg_note_queryadapter_history(uint32 type, uint32 size,
                                            int32 ret, uint32 status)
{
    uint32 slot = hvdxg.queryadapter_history_index %
                  HV_DXG_QUERY_HISTORY_MAX;
    uint32 head_word = 0;

    hvdxg.queryadapter_history_type[slot] = type;
    hvdxg.queryadapter_history_size[slot] = size;
    hvdxg.queryadapter_history_len[slot] = hvdxg.queryadapter_last_len;
    hvdxg.queryadapter_history_ret[slot] = ret;
    hvdxg.queryadapter_history_status[slot] = status;
    hvdxg.queryadapter_history_route[slot] = hvdxg.queryadapter_send_route;
    hvdxg.queryadapter_history_adapter[slot] =
        hvdxg.queryadapter_last_adapter;
    hvdxg.queryadapter_history_host_adapter[slot] =
        hvdxg.queryadapter_last_host_adapter;
    hvdxg.queryadapter_history_head_len[slot] =
        hvdxg.queryadapter_return_head_len;
    memset(hvdxg.queryadapter_history_head[slot], 0,
           sizeof(hvdxg.queryadapter_history_head[slot]));
    if (hvdxg.queryadapter_return_head_len != 0)
        memcpy(hvdxg.queryadapter_history_head[slot],
               hvdxg.queryadapter_return_head,
               hvdxg.queryadapter_return_head_len);
    hvdxg.queryadapter_history_index++;
    for (uint32 i = 0; i < hvdxg.queryadapter_return_head_len && i < 4; i++)
        head_word |= (uint32)hvdxg.queryadapter_return_head[i] << (i * 8);
    if (hvdxg_queryadapter_admission_type(type))
        hvdxg_note_queryadapter_admission(
            HV_DXG_QAI_ADMISSION_KIND_QAI, type, size,
            hvdxg.queryadapter_last_len, ret, status,
            hvdxg.queryadapter_send_route, hvdxg.queryadapter_source_last,
            hvdxg.queryadapter_last_adapter,
            hvdxg.queryadapter_last_host_adapter, head_word);
}

static void hvdxg_reset_queryadapter_return_head(void)
{
    hvdxg.queryadapter_return_head_len = 0;
    memset(hvdxg.queryadapter_return_head, 0,
           sizeof(hvdxg.queryadapter_return_head));
}

static void hvdxg_note_queryadapter_return_head(const void *payload,
                                                uint32 payload_len)
{
    uint32 head_len = payload_len;

    if (head_len > sizeof(hvdxg.queryadapter_return_head))
        head_len = sizeof(hvdxg.queryadapter_return_head);
    hvdxg_reset_queryadapter_return_head();
    hvdxg.queryadapter_return_head_len = head_len;
    if (payload != NULL && head_len != 0)
        memcpy(hvdxg.queryadapter_return_head, payload, head_len);
}

static int hvdxg_queryadapter_admission_type(uint32 type)
{
    return type == HV_DXG_QAITYPE_SELECTED_ADAPTER ||
           type == HV_DXG_QAITYPE_PHASE1_TYPE27 ||
           type == _KMTQAITYPE_ADAPTERTYPE ||
           type == _KMTQAITYPE_PHYSICALADAPTERCOUNT ||
           type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID ||
           type == HV_DXG_QAITYPE_CHECKDRIVERUPDATESTATUS_RENDER ||
           type == _KMTQAITYPE_ADAPTERTYPE_RENDER;
}

static void hvdxg_note_queryadapter_admission(uint32 kind, uint32 type,
                                              uint32 size, uint32 out_len,
                                              int32 ret, uint32 status,
                                              uint32 route, uint32 source,
                                              uint32 adapter,
                                              uint32 host_adapter,
                                              uint32 head_hash)
{
    uint32 slot = hvdxg.queryadapter_admission_history_index %
                  HV_DXG_QAI_ADMISSION_HISTORY_MAX;

    hvdxg.queryadapter_admission_kind[slot] = kind;
    hvdxg.queryadapter_admission_type[slot] = type;
    hvdxg.queryadapter_admission_size[slot] = size;
    hvdxg.queryadapter_admission_len[slot] = out_len;
    hvdxg.queryadapter_admission_ret[slot] = ret;
    hvdxg.queryadapter_admission_status[slot] = status;
    hvdxg.queryadapter_admission_route[slot] = route;
    hvdxg.queryadapter_admission_source[slot] = source;
    hvdxg.queryadapter_admission_adapter[slot] = adapter;
    hvdxg.queryadapter_admission_host[slot] = host_adapter;
    hvdxg.queryadapter_admission_head_hash[slot] = head_hash;
    hvdxg.queryadapter_admission_history_index++;
}

static uint32 hvdxg_read_u32(const void *ptr)
{
    uint32 value;

    memcpy(&value, ptr, sizeof(value));
    return value;
}

static int hvdxg_known_display_vendor(uint32 vendor)
{
    return vendor == HV_DXG_VENDOR_INTEL ||
           vendor == HV_DXG_VENDOR_NVIDIA ||
           vendor == 0x1002U;
}

static int hvdxg_sane_adapter_hardware_id(uint32 vendor, uint32 device)
{
    return vendor != 0 && vendor <= 0xffffU && device != 0;
}

static int hvdxg_synthetic_adapter_hardware_id(uint32 vendor, uint32 device)
{
    return vendor == PCI_VENDOR_MICROSOFT &&
           device == PCI_DEVICE_MS_VIRTUAL_RENDER;
}

static int hvdxg_real_adapter_hardware_id(uint32 vendor, uint32 device)
{
    return hvdxg_sane_adapter_hardware_id(vendor, device) &&
           !hvdxg_synthetic_adapter_hardware_id(vendor, device);
}

static int hvdxg_type31_active_v40_ext(void)
{
    return hvdxg.use_ext_header &&
           hvdxg.active_vmbus_version >= HV_DXG_VMBUS_INTERFACE_VERSION;
}

static int hvdxg_have_cached_adapter_hardware_id(void)
{
    return hvdxg.adapter_hardware_raw_size ==
               HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE &&
           hvdxg_real_adapter_hardware_id(hvdxg.adapter_vendor_id,
                                          hvdxg.adapter_device_id);
}

static void hvdxg_note_adapter_hardware_v40_short(uint32 actual_len,
                                                  int ret, int32 status)
{
    hvdxg.adapter_hardware_v40_short_zero++;
    hvdxg.adapter_hardware_v40_short_len = actual_len;
    hvdxg.adapter_hardware_v40_short_ret = ret;
    hvdxg.adapter_hardware_v40_short_status = status;
    hvdxg.adapter_hardware_cache_available =
        hvdxg_have_cached_adapter_hardware_id() ? 1 : 0;
    if (hvdxg_synthetic_adapter_hardware_id(hvdxg.pci_vendor,
                                            hvdxg.pci_dxg_device))
        hvdxg.adapter_hardware_synthetic_rejected = 1;
}

static const char *hvdxg_adapter_hardware_source_name(void)
{
    if (hvdxg.adapter_hardware_fallback_source == 1)
        return "cached-real";
    if (hvdxg.adapter_hardware_fallback_source == 2)
        return "legacy-noext-vgpu-query";
    if (hvdxg.adapter_hardware_fallback_source == 3)
        return "temp-v27-openadapter";
    if (!hvdxg.adapter_hardware_fallback &&
        hvdxg_real_adapter_hardware_id(hvdxg.adapter_vendor_id,
                                       hvdxg.adapter_device_id)) {
        if (hvdxg_type31_active_v40_ext())
            return "host-v40-type31";
        return "host-real";
    }
    if (hvdxg.adapter_hardware_synthetic_rejected)
        return "rejected-synthetic-pci";
    return "none";
}

static int hvdxg_normalize_adapter_hardware_id(uint8 *private_data,
                                               uint32 private_data_size)
{
    uint32 words[7];
    uint32 count;
    uint32 vendor;
    uint32 device;

    hvdxg.adapter_hardware_raw_size = private_data_size;
    hvdxg.adapter_hardware_payload_len = private_data_size;
    hvdxg.adapter_hardware_cache_available =
        hvdxg_have_cached_adapter_hardware_id() ? 1 : 0;
    hvdxg.adapter_hardware_normalized = 0;
    hvdxg.adapter_hardware_fallback = 0;
    hvdxg.adapter_hardware_fallback_source = 0;
    if (private_data_size < 12 || private_data == NULL)
        return 0;

    count = private_data_size / sizeof(uint32);
    if (count > sizeof(words) / sizeof(words[0]))
        count = sizeof(words) / sizeof(words[0]);
    memset(words, 0, sizeof(words));
    for (uint32 i = 0; i < count; i++)
        words[i] = hvdxg_read_u32(private_data + i * sizeof(uint32));

    /*
     * Type-31 PHYSICALADAPTERDEVICEIDS is an indexed payload on WSL:
     * word 0 is the physical-adapter index, word 1 is VendorID, and word 2
     * is DeviceID.  Keep the host bytes unchanged for user mode; diagnostics
     * keep normalized vendor/device separately.
     */
    if (!hvdxg_known_display_vendor(words[0]) &&
        hvdxg_known_display_vendor(words[1])) {
        hvdxg.adapter_hardware_normalized = 1;
        vendor = words[1];
        device = words[2];
    } else {
        vendor = words[0];
        device = words[1];
    }

    if (!hvdxg_real_adapter_hardware_id(vendor, device)) {
        if (hvdxg_synthetic_adapter_hardware_id(vendor, device))
            hvdxg.adapter_hardware_synthetic_rejected = 1;
        return 0;
    }

    memset(hvdxg.adapter_hardware_raw, 0,
           sizeof(hvdxg.adapter_hardware_raw));
    for (uint32 i = 0; i < count; i++)
        hvdxg.adapter_hardware_raw[i] = words[i];
    hvdxg.adapter_vendor_id = vendor;
    hvdxg.adapter_device_id = device;
    hvdxg.adapter_hardware_cache_available = 1;
    return 1;
}

static int hvdxg_accept_legacy_adapter_hardware_id(uint8 *result_buf,
                                                   uint32 actual_len,
                                                   uint32 private_data_size)
{
    struct hvdxg_ntstatus *status;
    uint8 *payload;
    int status_ret;

    if (result_buf == NULL ||
        private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE)
        return 0;

    if (actual_len >= private_data_size + sizeof(*status) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)result_buf)) {
        status = (struct hvdxg_ntstatus *)result_buf;
        status_ret = hvdxg_ntstatus_to_errno(*status);
        if (status_ret != 0)
            return 0;
        payload = result_buf + sizeof(*status);
    } else if (actual_len >= private_data_size) {
        payload = result_buf;
    } else {
        return 0;
    }

    if (!hvdxg_normalize_adapter_hardware_id(payload, private_data_size))
        return 0;
    if (payload != result_buf)
        memmove(result_buf, payload, private_data_size);
    hvdxg.adapter_hardware_fallback = 1;
    hvdxg.adapter_hardware_fallback_count++;
    hvdxg.adapter_hardware_fallback_source = 2;
    return 1;
}

static int hvdxg_adapter_hardware_primary_too_short(uint8 *result_buf,
                                                    uint32 actual_len,
                                                    uint32 private_data_size)
{
    if (private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE)
        return 0;
    if (actual_len < private_data_size)
        return 1;
    if (actual_len > private_data_size &&
        actual_len < private_data_size + sizeof(struct hvdxg_ntstatus) &&
        actual_len >= sizeof(struct hvdxg_ntstatus) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)result_buf))
        return 1;
    return 0;
}

static int hvdxg_queryadapter_type55_zero_completion(uint32 type, uint32 size,
                                                     uint32 actual_len,
                                                     int ret)
{
    (void)size;
    return type == HV_DXG_QAITYPE_CHECKDRIVERUPDATESTATUS_RENDER &&
           actual_len == 0 && ret == 0;
}

static void hvdxg_note_queryadapter_zero_success(uint32 type, uint32 size,
                                                 uint32 host_len,
                                                 int host_ret,
                                                 int32 host_status,
                                                 int user_ret)
{
    hvdxg.queryadapter_zero_success_type = type;
    hvdxg.queryadapter_zero_success_size = size;
    hvdxg.queryadapter_zero_success_count++;
    hvdxg.queryadapter_zero_success_host_type = type;
    hvdxg.queryadapter_zero_success_host_len = host_len;
    hvdxg.queryadapter_zero_success_host_ret = host_ret;
    hvdxg.queryadapter_zero_success_host_status = host_status;
    hvdxg.queryadapter_zero_success_user_ret = user_ret;
}

static int hvdxg_queryadapter_adaptertype_zero_completion(uint32 type,
                                                          uint32 size,
                                                          uint32 actual_len,
                                                          int ret)
{
    return (type == _KMTQAITYPE_ADAPTERTYPE ||
            type == _KMTQAITYPE_ADAPTERTYPE_RENDER) &&
           size >= sizeof(struct d3dkmt_adaptertype) &&
           actual_len == 0 && ret == 0;
}

static int hvdxg_queryadapter_physicalcount_fallback(uint32 type,
                                                     uint32 size,
                                                     uint32 actual_len,
                                                     int ret)
{
    return type == _KMTQAITYPE_PHYSICALADAPTERCOUNT &&
           size >= sizeof(uint32) && actual_len == 0 && ret == -EOVERFLOW;
}

static int hvdxg_try_fallback_adapter_hardware_id(uint8 *private_data,
                                                  uint32 private_data_size)
{
    uint32 words[7];
    uint32 vendor;
    uint32 device;
    uint32 count;

    if (private_data == NULL ||
        private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE)
        return 0;
    if (hvdxg_synthetic_adapter_hardware_id(hvdxg.pci_vendor,
                                            hvdxg.pci_dxg_device))
        hvdxg.adapter_hardware_synthetic_rejected = 1;
    hvdxg.adapter_hardware_cache_available =
        hvdxg_have_cached_adapter_hardware_id() ? 1 : 0;
    if (!hvdxg.adapter_hardware_cache_available)
        return 0;

    vendor = hvdxg.adapter_vendor_id;
    device = hvdxg.adapter_device_id;

    memset(words, 0, sizeof(words));
    count = sizeof(hvdxg.adapter_hardware_raw) /
            sizeof(hvdxg.adapter_hardware_raw[0]);
    for (uint32 i = 0; i < count; i++)
        words[i] = hvdxg.adapter_hardware_raw[i];
    if (hvdxg_known_display_vendor(words[0])) {
        words[6] = words[5];
        words[5] = words[4];
        words[4] = words[3];
        words[3] = words[2];
        words[2] = device;
        words[1] = vendor;
        words[0] = 0;
    }
    memcpy(private_data, words, sizeof(words));

    memset(hvdxg.adapter_hardware_raw, 0,
           sizeof(hvdxg.adapter_hardware_raw));
    for (uint32 i = 0; i < count; i++)
        hvdxg.adapter_hardware_raw[i] = words[i];
    hvdxg.adapter_hardware_raw_size = private_data_size;
    hvdxg.adapter_hardware_payload_len = private_data_size;
    hvdxg.adapter_hardware_normalized = 2;
    hvdxg.adapter_hardware_fallback = 1;
    hvdxg.adapter_hardware_fallback_count++;
    hvdxg.adapter_hardware_fallback_source = 1;
    hvdxg.adapter_vendor_id = vendor;
    hvdxg.adapter_device_id = device;
    return 1;
}

#if 0
static int hvdxg_cache_v27_adapter_hardware_id_before_v40(void)
{
    enum {
        pre_cache_command_len =
            HV_DXG_QUERYADAPTERINFO_WSL_NATURAL_BASE +
            HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE - 1U,
        pre_cache_result_len =
            sizeof(struct hvdxg_ntstatus) +
            HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE,
    };
    struct hvdxg_command_openadapter open;
    struct hvdxg_command_openadapter_return open_ret;
    struct hvdxg_command_closeadapter close;
    struct hvdxg_ntstatus close_status;
    uint8 command_buf[pre_cache_command_len];
    uint8 result_buf[pre_cache_result_len];
    struct hvdxg_command_queryadapterinfo_wsl *query =
        (struct hvdxg_command_queryadapterinfo_wsl *)command_buf;
    uint32 saved_host_adapter_handle = hvdxg.host_adapter_handle;
    struct hvdxg_winluid saved_adapter_luid = hvdxg.adapter_luid;
    struct hvdxg_winluid saved_host_adapter_luid = hvdxg.host_adapter_luid;
    struct hvdxg_winluid saved_host_vgpu_luid = hvdxg.host_vgpu_luid;
    uint32 saved_active_version = hvdxg.active_vmbus_version;
    uint32 saved_active_source = hvdxg.active_vmbus_source;
    uint32 saved_active_compat = hvdxg.active_vmbus_last_compat;
    uint32 saved_adapter_vendor_id = hvdxg.adapter_vendor_id;
    uint32 saved_adapter_device_id = hvdxg.adapter_device_id;
    uint32 saved_hardware_raw[7];
    uint32 saved_hardware_raw_size = hvdxg.adapter_hardware_raw_size;
    uint32 saved_hardware_normalized = hvdxg.adapter_hardware_normalized;
    uint32 saved_hardware_fallback = hvdxg.adapter_hardware_fallback;
    uint32 saved_hardware_fallback_count =
        hvdxg.adapter_hardware_fallback_count;
    uint32 saved_hardware_fallback_source =
        hvdxg.adapter_hardware_fallback_source;
    uint32 saved_hardware_synthetic_rejected =
        hvdxg.adapter_hardware_synthetic_rejected;
    uint32 saved_hardware_cache_available =
        hvdxg.adapter_hardware_cache_available;
    uint32 saved_hardware_payload_len =
        hvdxg.adapter_hardware_payload_len;
    uint32 temp_handle = 0;
    uint32 actual_len = 0;
    uint32 close_actual = 0;
    uint32 words[7];
    uint8 *payload = NULL;
    int success = 0;
    int ret = -EIO;

    hvdxg.adapter_hardware_temp_v27_attempts++;
    hvdxg.adapter_hardware_temp_v27_last_ret = 0;
    hvdxg.adapter_hardware_temp_v27_last_status = 0;
    hvdxg.adapter_hardware_temp_v27_open_len = 0;
    hvdxg.adapter_hardware_temp_v27_query_len = 0;
    hvdxg.adapter_hardware_temp_v27_close_len = 0;
    hvdxg.adapter_hardware_temp_v27_close_ret = 0;
    hvdxg.adapter_hardware_temp_v27_close_status = 0;
    hvdxg.adapter_hardware_temp_v27_handle = 0;
    hvdxg.adapter_hardware_temp_v27_restored_version = 0;
    hvdxg.adapter_hardware_temp_v27_restored_ext = 0;

    if (!hvdxg_host_v40_signal()) {
        ret = -ENODEV;
        goto finish;
    }
    if (hvdxg.adapter_hardware_fallback_source == 4 &&
        hvdxg.adapter_hardware_raw_size ==
            HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE &&
        hvdxg_real_adapter_hardware_id(hvdxg.adapter_vendor_id,
                                       hvdxg.adapter_device_id)) {
        success = 1;
        ret = 0;
        goto finish;
    }
    if (!hvdxg.vgpu_open_ok || hvdxg.vgpu_out_ring == NULL) {
        ret = -ENODEV;
        goto finish;
    }

    memcpy(saved_hardware_raw, hvdxg.adapter_hardware_raw,
           sizeof(saved_hardware_raw));
    hvdxg_set_active_vmbus_version(HV_DXG_VMBUS_INTERFACE_VERSION_OLD,
                                   5,
                                   HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION);

    memset(&open, 0, sizeof(open));
    hvdxg_command_vgpu_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENADAPTER);
    open.vmbus_interface_version = HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
    open.vmbus_last_compatible_interface_version =
        HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;
    if (hvdxg_luid_nonzero(saved_adapter_luid))
        open.guest_adapter_luid = saved_adapter_luid;
    else
        open.guest_adapter_luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);

    memset(&open_ret, 0, sizeof(open_ret));
    ret = hvdxg_send_sync_vgpu_flags(
        &open, sizeof(open), &open_ret, sizeof(open_ret), &actual_len,
        HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
        HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
    hvdxg.adapter_hardware_temp_v27_open_len = actual_len;
    if (ret != 0)
        goto restore;
    if (actual_len < sizeof(open_ret)) {
        ret = -EOVERFLOW;
        goto restore;
    }
    hvdxg.adapter_hardware_temp_v27_last_status = open_ret.status.v;
    if (open_ret.status.v < 0) {
        ret = hvdxg_ntstatus_to_errno(open_ret.status);
        goto restore;
    }
    temp_handle = open_ret.host_adapter_handle.v;
    hvdxg.adapter_hardware_temp_v27_handle = temp_handle;
    if (temp_handle == 0) {
        ret = -ENODEV;
        goto restore;
    }

    memset(command_buf, 0, sizeof(command_buf));
    query->hdr.process = hvdxg.dxg_process;
    query->hdr.channel_type = HV_DXGKVMB_VGPU_TO_HOST;
    query->hdr.command_type = HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO;
    query->query_type = HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID;
    query->private_data_size = HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE;

    memset(result_buf, 0, sizeof(result_buf));
    actual_len = 0;
    ret = hvdxg_send_sync_vgpu_flags(
        query, pre_cache_command_len, result_buf, sizeof(result_buf),
        &actual_len,
        HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
        HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
    hvdxg.adapter_hardware_temp_v27_query_len = actual_len;
    if (actual_len >= sizeof(struct hvdxg_ntstatus) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)result_buf))
        hvdxg.adapter_hardware_temp_v27_last_status =
            ((struct hvdxg_ntstatus *)result_buf)->v;
    if (ret != 0)
        goto close_temp;

    if (actual_len == HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE) {
        payload = result_buf;
    } else if (actual_len == pre_cache_result_len &&
               hvdxg_ntstatus_plausible(
                   *(struct hvdxg_ntstatus *)result_buf)) {
        struct hvdxg_ntstatus *status =
            (struct hvdxg_ntstatus *)result_buf;

        ret = hvdxg_ntstatus_to_errno(*status);
        if (ret != 0)
            goto close_temp;
        payload = result_buf + sizeof(*status);
    } else {
        ret = actual_len < HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE ?
              -EOVERFLOW : -EINVAL;
        goto close_temp;
    }

    memset(words, 0, sizeof(words));
    for (uint32 i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        words[i] = hvdxg_read_u32(payload + i * sizeof(uint32));
    if (hvdxg_known_display_vendor(words[0])) {
        uint32 vendor = words[0];
        uint32 device = words[1];

        words[6] = words[5];
        words[5] = words[4];
        words[4] = words[3];
        words[3] = words[2];
        words[2] = device;
        words[1] = vendor;
        words[0] = 0;
    }
    if (!hvdxg_normalize_adapter_hardware_id((uint8 *)words,
                                             sizeof(words))) {
        ret = -EINVAL;
        goto close_temp;
    }
    hvdxg.adapter_hardware_fallback = 0;
    hvdxg.adapter_hardware_fallback_count =
        saved_hardware_fallback_count;
    hvdxg.adapter_hardware_fallback_source = 4;
    success = 1;

close_temp:
    if (temp_handle != 0 && temp_handle != saved_host_adapter_handle) {
        memset(&close, 0, sizeof(close));
        memset(&close_status, 0, sizeof(close_status));
        hvdxg_command_vgpu_init(&close.hdr,
                                HV_DXGK_VMBCOMMAND_CLOSEADAPTER);
        close.host_handle.v = temp_handle;
        hvdxg.adapter_hardware_temp_v27_close_ret =
            hvdxg_send_sync_vgpu_flags(
                &close, sizeof(close), &close_status,
                sizeof(close_status), &close_actual,
                HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
                HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
        hvdxg.adapter_hardware_temp_v27_close_len = close_actual;
        if (close_actual >= sizeof(close_status))
            hvdxg.adapter_hardware_temp_v27_close_status =
                close_status.v;
    }

restore:
    hvdxg.host_adapter_handle = saved_host_adapter_handle;
    hvdxg.adapter_luid = saved_adapter_luid;
    hvdxg.host_adapter_luid = saved_host_adapter_luid;
    hvdxg.host_vgpu_luid = saved_host_vgpu_luid;
    hvdxg_set_active_vmbus_version(saved_active_version,
                                   saved_active_source,
                                   saved_active_compat);
    if (!success) {
        hvdxg.adapter_vendor_id = saved_adapter_vendor_id;
        hvdxg.adapter_device_id = saved_adapter_device_id;
        memcpy(hvdxg.adapter_hardware_raw, saved_hardware_raw,
               sizeof(hvdxg.adapter_hardware_raw));
        hvdxg.adapter_hardware_raw_size = saved_hardware_raw_size;
        hvdxg.adapter_hardware_normalized = saved_hardware_normalized;
        hvdxg.adapter_hardware_fallback = saved_hardware_fallback;
        hvdxg.adapter_hardware_fallback_count =
            saved_hardware_fallback_count;
        hvdxg.adapter_hardware_fallback_source =
            saved_hardware_fallback_source;
        hvdxg.adapter_hardware_synthetic_rejected =
            saved_hardware_synthetic_rejected;
        hvdxg.adapter_hardware_cache_available =
            saved_hardware_cache_available;
        hvdxg.adapter_hardware_payload_len =
            saved_hardware_payload_len;
    }
finish:
    hvdxg.adapter_hardware_temp_v27_restored_version =
        hvdxg.active_vmbus_version;
    hvdxg.adapter_hardware_temp_v27_restored_ext = hvdxg.use_ext_header;
    hvdxg.adapter_hardware_temp_v27_last_ret = success ? 0 : ret;
    if (success)
        hvdxg.adapter_hardware_temp_v27_successes++;
    else
        hvdxg.adapter_hardware_temp_v27_failures++;
    return success ? 1 : ret;
}
#endif

static int hvdxg_try_v27_openadapter_hardware_id(
    struct hvdxg_open_state *owner,
    struct hvdxg_command_queryadapterinfo_wsl *query, uint32 command_len,
    uint8 *result_buf, uint32 result_len, uint32 private_data_size,
    uint32 *actual_len)
{
    struct hvdxg_command_openadapter open;
    struct hvdxg_command_openadapter_return open_ret;
    struct hvdxg_command_closeadapter close;
    struct hvdxg_ntstatus close_status;
    uint8 temp_result[sizeof(struct hvdxg_ntstatus) +
                      HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE];
    uint32 saved_host_adapter_handle = hvdxg.host_adapter_handle;
    struct hvdxg_winluid saved_adapter_luid = hvdxg.adapter_luid;
    struct hvdxg_winluid saved_host_adapter_luid = hvdxg.host_adapter_luid;
    struct hvdxg_winluid saved_host_vgpu_luid = hvdxg.host_vgpu_luid;
    uint32 saved_adapter_vendor_id = hvdxg.adapter_vendor_id;
    uint32 saved_adapter_device_id = hvdxg.adapter_device_id;
    uint32 saved_hardware_raw[7];
    uint32 saved_hardware_raw_size = hvdxg.adapter_hardware_raw_size;
    uint32 saved_hardware_normalized = hvdxg.adapter_hardware_normalized;
    uint32 saved_hardware_fallback = hvdxg.adapter_hardware_fallback;
    uint32 saved_hardware_fallback_count =
        hvdxg.adapter_hardware_fallback_count;
    uint32 saved_hardware_fallback_source =
        hvdxg.adapter_hardware_fallback_source;
    uint32 saved_hardware_synthetic_rejected =
        hvdxg.adapter_hardware_synthetic_rejected;
    uint32 saved_hardware_cache_available =
        hvdxg.adapter_hardware_cache_available;
    uint32 saved_hardware_payload_len =
        hvdxg.adapter_hardware_payload_len;
    uint32 temp_handle = 0;
    uint32 open_actual = 0;
    uint32 query_actual = 0;
    uint32 close_actual = 0;
    uint32 words[7];
    uint8 *payload = NULL;
    int success = 0;
    int ret = -EINVAL;

    memcpy(saved_hardware_raw, hvdxg.adapter_hardware_raw,
           sizeof(saved_hardware_raw));
    hvdxg.adapter_hardware_temp_v27_attempts++;
    hvdxg.adapter_hardware_temp_v27_last_ret = 0;
    hvdxg.adapter_hardware_temp_v27_last_status = 0;
    hvdxg.adapter_hardware_temp_v27_open_len = 0;
    hvdxg.adapter_hardware_temp_v27_query_len = 0;
    hvdxg.adapter_hardware_temp_v27_close_len = 0;
    hvdxg.adapter_hardware_temp_v27_close_ret = 0;
    hvdxg.adapter_hardware_temp_v27_close_status = 0;
    hvdxg.adapter_hardware_temp_v27_handle = 0;
    hvdxg.adapter_hardware_temp_v27_restored_version = 0;
    hvdxg.adapter_hardware_temp_v27_restored_ext = 0;

    if (query == NULL || result_buf == NULL || actual_len == NULL ||
        private_data_size != HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE ||
        result_len < private_data_size + sizeof(struct hvdxg_ntstatus)) {
        ret = -EINVAL;
        goto restore;
    }
    if (!hvdxg.vgpu_open_ok || hvdxg.vgpu_out_ring == NULL ||
        hvdxg.dxg_process.v == 0) {
        ret = -ENODEV;
        goto restore;
    }
    if (owner != NULL &&
        (owner->device_count != 0 || owner->context_count != 0 ||
         owner->hwqueue_count != 0 || owner->allocation_count != 0)) {
        ret = -EBUSY;
        goto restore;
    }

    memset(&open, 0, sizeof(open));
    hvdxg_command_vgpu_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENADAPTER);
    open.vmbus_interface_version = HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
    open.vmbus_last_compatible_interface_version =
        HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;
    if (hvdxg_luid_nonzero(saved_adapter_luid))
        open.guest_adapter_luid = saved_adapter_luid;
    else
        open.guest_adapter_luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);

    memset(&open_ret, 0, sizeof(open_ret));
    ret = hvdxg_send_sync_vgpu_flags(
        &open, sizeof(open), &open_ret, sizeof(open_ret), &open_actual,
        HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
        HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
    hvdxg.adapter_hardware_temp_v27_open_len = open_actual;
    if (ret != 0)
        goto restore;
    if (open_actual < sizeof(open_ret)) {
        ret = -EOVERFLOW;
        goto restore;
    }
    hvdxg.adapter_hardware_temp_v27_last_status = open_ret.status.v;
    if (open_ret.status.v < 0) {
        ret = hvdxg_ntstatus_to_errno(open_ret.status);
        goto close_temp;
    }
    temp_handle = open_ret.host_adapter_handle.v;
    hvdxg.adapter_hardware_temp_v27_handle = temp_handle;
    if (temp_handle == 0) {
        ret = -ENODEV;
        goto close_temp;
    }

    memset(temp_result, 0, sizeof(temp_result));
    ret = hvdxg_send_sync_vgpu_flags(
        query, command_len, temp_result, sizeof(temp_result), &query_actual,
        HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
        HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
    hvdxg.adapter_hardware_temp_v27_query_len = query_actual;
    if (actual_len != NULL)
        *actual_len = query_actual;
    if (query_actual >= sizeof(struct hvdxg_ntstatus) &&
        hvdxg_ntstatus_plausible(*(struct hvdxg_ntstatus *)temp_result))
        hvdxg.adapter_hardware_temp_v27_last_status =
            ((struct hvdxg_ntstatus *)temp_result)->v;
    if (ret != 0)
        goto close_temp;

    if (query_actual == private_data_size) {
        payload = temp_result;
    } else if (query_actual == private_data_size +
                            sizeof(struct hvdxg_ntstatus) &&
               hvdxg_ntstatus_plausible(
                   *(struct hvdxg_ntstatus *)temp_result)) {
        struct hvdxg_ntstatus *status =
            (struct hvdxg_ntstatus *)temp_result;

        ret = hvdxg_ntstatus_to_errno(*status);
        if (ret != 0)
            goto close_temp;
        payload = temp_result + sizeof(*status);
    } else {
        ret = query_actual < private_data_size ? -EOVERFLOW : -EINVAL;
        goto close_temp;
    }

    memset(words, 0, sizeof(words));
    for (uint32 i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        words[i] = hvdxg_read_u32(payload + i * sizeof(uint32));
    if (hvdxg_known_display_vendor(words[0])) {
        uint32 vendor = words[0];
        uint32 device = words[1];

        words[6] = words[5];
        words[5] = words[4];
        words[4] = words[3];
        words[3] = words[2];
        words[2] = device;
        words[1] = vendor;
        words[0] = 0;
    }
    memcpy(result_buf, words, sizeof(words));
    if (!hvdxg_normalize_adapter_hardware_id(result_buf,
                                             private_data_size)) {
        ret = -EINVAL;
        goto close_temp;
    }
    success = 1;
    ret = 0;

close_temp:
    if (temp_handle != 0 && temp_handle != saved_host_adapter_handle) {
        memset(&close, 0, sizeof(close));
        memset(&close_status, 0, sizeof(close_status));
        hvdxg_command_vgpu_init(&close.hdr,
                                HV_DXGK_VMBCOMMAND_CLOSEADAPTER);
        close.host_handle.v = temp_handle;
        hvdxg.adapter_hardware_temp_v27_close_ret =
            hvdxg_send_sync_vgpu_flags(
                &close, sizeof(close), &close_status, sizeof(close_status),
                &close_actual,
                HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER |
                HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU);
        hvdxg.adapter_hardware_temp_v27_close_len = close_actual;
        if (close_actual >= sizeof(close_status))
            hvdxg.adapter_hardware_temp_v27_close_status =
                close_status.v;
    }

restore:
    hvdxg.host_adapter_handle = saved_host_adapter_handle;
    hvdxg.adapter_luid = saved_adapter_luid;
    hvdxg.host_adapter_luid = saved_host_adapter_luid;
    hvdxg.host_vgpu_luid = saved_host_vgpu_luid;
    hvdxg.adapter_vendor_id = saved_adapter_vendor_id;
    hvdxg.adapter_device_id = saved_adapter_device_id;
    memcpy(hvdxg.adapter_hardware_raw, saved_hardware_raw,
           sizeof(hvdxg.adapter_hardware_raw));
    hvdxg.adapter_hardware_raw_size = saved_hardware_raw_size;
    hvdxg.adapter_hardware_normalized = saved_hardware_normalized;
    hvdxg.adapter_hardware_fallback = saved_hardware_fallback;
    hvdxg.adapter_hardware_fallback_count =
        saved_hardware_fallback_count;
    hvdxg.adapter_hardware_fallback_source =
        saved_hardware_fallback_source;
    hvdxg.adapter_hardware_synthetic_rejected =
        saved_hardware_synthetic_rejected;
    hvdxg.adapter_hardware_cache_available =
        saved_hardware_cache_available;
    hvdxg.adapter_hardware_payload_len =
        saved_hardware_payload_len;
    hvdxg.adapter_hardware_temp_v27_restored_version =
        hvdxg.active_vmbus_version;
    hvdxg.adapter_hardware_temp_v27_restored_ext =
        hvdxg.use_ext_header;

    if (success) {
        if (!hvdxg_normalize_adapter_hardware_id(result_buf,
                                                 private_data_size)) {
            ret = -EINVAL;
            success = 0;
        } else {
            hvdxg.adapter_hardware_fallback = 1;
            hvdxg.adapter_hardware_fallback_count =
                saved_hardware_fallback_count + 1;
            hvdxg.adapter_hardware_fallback_source = 3;
        }
    }
    hvdxg.adapter_hardware_temp_v27_last_ret = ret;
    if (success)
        hvdxg.adapter_hardware_temp_v27_successes++;
    else
        hvdxg.adapter_hardware_temp_v27_failures++;
    return success;
}

static int hvdxg_try_adapter_hardware_failure_fallbacks(
    struct hvdxg_open_state *owner,
    struct hvdxg_command_queryadapterinfo_wsl *query, uint32 command_len,
    uint8 *result_buf, uint32 result_len, uint32 private_data_size,
    uint32 *actual_len, int allow_cached, int allow_temp_v27)
{
    uint32 temp_actual = 0;

    if (allow_cached &&
        hvdxg_try_fallback_adapter_hardware_id(result_buf,
                                               private_data_size)) {
        if (actual_len != NULL)
            *actual_len = private_data_size;
        return 1;
    }
    if (allow_temp_v27 &&
        hvdxg_try_v27_openadapter_hardware_id(owner, query, command_len,
            result_buf, result_len, private_data_size, &temp_actual)) {
        if (actual_len != NULL)
            *actual_len = private_data_size;
        return 1;
    }
    return 0;
}

static void hvdxg_note_allocation_history(uint32 len, int32 ret,
                                          uint32 device, uint32 resource,
                                          uint32 allocation, uint64 size,
                                          uint32 count, uint32 priv_size,
                                          uint32 global_share,
                                          uint32 create_flags)
{
    uint32 slot = hvdxg.allocation_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;

    hvdxg.allocation_history_len[slot] = len;
    hvdxg.allocation_history_ret[slot] = ret;
    hvdxg.allocation_history_device[slot] = device;
    hvdxg.allocation_history_resource[slot] = resource;
    hvdxg.allocation_history_allocation[slot] = allocation;
    hvdxg.allocation_history_size[slot] = size;
    hvdxg.allocation_history_count[slot] = count;
    hvdxg.allocation_history_priv[slot] = priv_size;
    hvdxg.allocation_history_global_share[slot] = global_share;
    hvdxg.allocation_history_create_flags[slot] = create_flags;
    hvdxg.allocation_history_index++;
}

static void hvdxg_note_mapgpuva_history(uint32 len, int32 ret, uint32 status,
                                        uint32 paging_queue,
                                        uint32 allocation, uint64 pages,
                                        uint64 protection,
                                        uint64 driver_protection,
                                        uint64 va, uint64 fence)
{
    uint32 slot = hvdxg.mapgpuva_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;

    hvdxg.mapgpuva_history_len[slot] = len;
    hvdxg.mapgpuva_history_ret[slot] = ret;
    hvdxg.mapgpuva_history_status[slot] = status;
    hvdxg.mapgpuva_history_paging_queue[slot] = paging_queue;
    hvdxg.mapgpuva_history_allocation[slot] = allocation;
    hvdxg.mapgpuva_history_pages[slot] = pages;
    hvdxg.mapgpuva_history_protection[slot] = protection;
    hvdxg.mapgpuva_history_driver_protection[slot] = driver_protection;
    hvdxg.mapgpuva_history_va[slot] = va;
    hvdxg.mapgpuva_history_fence[slot] = fence;
    hvdxg.mapgpuva_history_index++;
}

static void hvdxg_note_submithwqueue_history(uint32 len, int32 ret,
                                             uint32 queue,
                                             uint32 command_length,
                                             uint32 priv_size,
                                             const uint8 *priv_head,
                                             uint32 priv_head_len)
{
    uint32 slot = hvdxg.submithwqueue_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;
    uint32 head_len = priv_head_len < 8 ? priv_head_len : 8;

    hvdxg.submithwqueue_history_len[slot] = len;
    hvdxg.submithwqueue_history_ret[slot] = ret;
    hvdxg.submithwqueue_history_queue[slot] = queue;
    hvdxg.submithwqueue_history_command_length[slot] = command_length;
    hvdxg.submithwqueue_history_priv_size[slot] = priv_size;
    hvdxg.submithwqueue_history_priv_head_len[slot] = head_len;
    memset(hvdxg.submithwqueue_history_priv_head[slot], 0,
           sizeof(hvdxg.submithwqueue_history_priv_head[slot]));
    if (head_len != 0 && priv_head != NULL)
        memcpy(hvdxg.submithwqueue_history_priv_head[slot], priv_head,
               head_len);
    hvdxg.submithwqueue_history_index++;
}

static void hvdxg_note_lock2_history(uint32 len, int32 ret, uint32 status,
                                     uint32 device, uint32 allocation,
                                     uint64 offset, uint64 user_va,
                                     uint64 map_size)
{
    uint32 slot = hvdxg.lock2_history_index %
                  HV_DXG_RESOURCE_HISTORY_MAX;

    hvdxg.lock2_history_len[slot] = len;
    hvdxg.lock2_history_ret[slot] = ret;
    hvdxg.lock2_history_status[slot] = status;
    hvdxg.lock2_history_device[slot] = device;
    hvdxg.lock2_history_allocation[slot] = allocation;
    hvdxg.lock2_history_offset[slot] = offset;
    hvdxg.lock2_history_user_va[slot] = user_va;
    hvdxg.lock2_history_map_size[slot] = map_size;
    hvdxg.lock2_history_index++;
}

#define HV_DXG_QH_ARGS(i) \
    hvdxg.queryadapter_history_type[(i)], \
    hvdxg.queryadapter_history_size[(i)], \
    hvdxg.queryadapter_history_len[(i)], \
    hvdxg.queryadapter_history_ret[(i)], \
    hvdxg.queryadapter_history_status[(i)]

#define HV_DXG_QAI_ADMISSION_ARGS(i) \
    hvdxg.queryadapter_admission_kind[(i)], \
    hvdxg.queryadapter_admission_type[(i)], \
    hvdxg.queryadapter_admission_size[(i)], \
    hvdxg.queryadapter_admission_len[(i)], \
    hvdxg.queryadapter_admission_ret[(i)], \
    hvdxg.queryadapter_admission_status[(i)], \
    hvdxg.queryadapter_admission_route[(i)], \
    hvdxg.queryadapter_admission_source[(i)], \
    hvdxg.queryadapter_admission_adapter[(i)], \
    hvdxg.queryadapter_admission_host[(i)], \
    hvdxg.queryadapter_admission_head_hash[(i)]

#define HV_DXG_AH_ARGS(i) \
    hvdxg.allocation_history_device[(i)], \
    hvdxg.allocation_history_resource[(i)], \
    hvdxg.allocation_history_allocation[(i)], \
    hvdxg.allocation_history_size[(i)], \
    hvdxg.allocation_history_len[(i)], \
    hvdxg.allocation_history_ret[(i)], \
    hvdxg.allocation_history_count[(i)], \
    hvdxg.allocation_history_priv[(i)]

#define HV_DXG_MH_ARGS(i) \
    hvdxg.mapgpuva_history_allocation[(i)], \
    hvdxg.mapgpuva_history_va[(i)], \
    hvdxg.mapgpuva_history_pages[(i)], \
    hvdxg.mapgpuva_history_fence[(i)], \
    hvdxg.mapgpuva_history_len[(i)], \
    hvdxg.mapgpuva_history_ret[(i)], \
    hvdxg.mapgpuva_history_status[(i)], \
    hvdxg.mapgpuva_history_paging_queue[(i)]

#define HV_DXG_LH_ARGS(i) \
    hvdxg.lock2_history_device[(i)], \
    hvdxg.lock2_history_allocation[(i)], \
    hvdxg.lock2_history_offset[(i)], \
    hvdxg.lock2_history_user_va[(i)], \
    hvdxg.lock2_history_map_size[(i)], \
    hvdxg.lock2_history_len[(i)], \
    hvdxg.lock2_history_ret[(i)], \
    hvdxg.lock2_history_status[(i)]

#define HV_DXG_SH_ARGS(i) \
    hvdxg.submithwqueue_history_len[(i)], \
    hvdxg.submithwqueue_history_ret[(i)], \
    hvdxg.submithwqueue_history_queue[(i)], \
    hvdxg.submithwqueue_history_command_length[(i)], \
    hvdxg.submithwqueue_history_priv_size[(i)], \
    hvdxg.submithwqueue_history_priv_head_len[(i)], \
    hvdxg.submithwqueue_history_priv_head[(i)][0], \
    hvdxg.submithwqueue_history_priv_head[(i)][1], \
    hvdxg.submithwqueue_history_priv_head[(i)][2], \
    hvdxg.submithwqueue_history_priv_head[(i)][3], \
    hvdxg.submithwqueue_history_priv_head[(i)][4], \
    hvdxg.submithwqueue_history_priv_head[(i)][5], \
    hvdxg.submithwqueue_history_priv_head[(i)][6], \
    hvdxg.submithwqueue_history_priv_head[(i)][7]

static void hvdxg_note_ioctl_history(uint64 cmd, int ret)
{
    uint32 slot = hvdxg.ioctl_history_index %
                  HV_DXG_IOCTL_HISTORY_MAX;

    hvdxg.ioctl_history_cmd[slot] = (uint32)cmd;
    hvdxg.ioctl_history_nr[slot] = (uint32)(cmd & 0xffU);
    hvdxg.ioctl_history_ret[slot] = ret;
    hvdxg.ioctl_history_index++;
}

static uint64 hvdxg_ticks_to_us(uint64 ticks)
{
    uint64 freq = __timebase_frequency;

    if (freq == 0)
        return ticks;
    return (ticks / freq) * 1000000ULL +
           ((ticks % freq) * 1000000ULL) / freq;
}

static void hvdxg_note_ioctl_timing(uint64 cmd, uint64 ticks)
{
    uint32 nr = (uint32)(cmd & 0xffU);

    hvdxg.ioctl_nr_count[nr]++;
    hvdxg.ioctl_nr_total_ticks[nr] += ticks;
    hvdxg.ioctl_nr_last_ticks[nr] = ticks;
    if (ticks > hvdxg.ioctl_nr_max_ticks[nr])
        hvdxg.ioctl_nr_max_ticks[nr] = ticks;
}

static void hvdxg_note_host_command(uint32 command_type)
{
    uint32 order = 0;

    switch (command_type) {
    case HV_DXGK_VMBCOMMAND_DESTROYALLOCATION:
        hvdxg.host_cmd_destroyallocation++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYCONTEXT:
        hvdxg.host_cmd_destroycontext++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE:
        hvdxg.host_cmd_destroyhwqueue++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE:
        hvdxg.host_cmd_destroypagingqueue++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYDEVICE:
        hvdxg.host_cmd_destroydevice++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT:
        hvdxg.host_cmd_destroysync++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_DESTROYPROCESS:
        hvdxg.host_cmd_destroyprocess++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_destroyprocess_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS:
        hvdxg.host_cmd_freegpuva++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.cleanup_last_destroy_order = order;
        break;
    case HV_DXGK_VMBCOMMAND_CLOSEADAPTER:
        hvdxg.closeadapter_host_count++;
        order = ++hvdxg.cleanup_order_seq;
        hvdxg.closeadapter_last_order = order;
        if (hvdxg.cleanup_last_destroy_order != 0 &&
            order > hvdxg.cleanup_last_destroy_order)
            hvdxg.closeadapter_after_destroy_count++;
        break;
    case HV_DXGK_VMBCOMMAND_LOCK2:
        hvdxg.host_cmd_lock2++;
        break;
    case HV_DXGK_VMBCOMMAND_UNLOCK2:
        hvdxg.host_cmd_unlock2++;
        break;
    default:
        break;
    }
}

static void hvdxg_ioctl_timing_top(uint32 *top_nr)
{
    uint64 top_ticks[HV_DXG_IOCTL_TIME_TOP];

    memset(top_nr, 0, sizeof(uint32) * HV_DXG_IOCTL_TIME_TOP);
    memset(top_ticks, 0, sizeof(top_ticks));
    for (uint32 nr = 0; nr < HV_DXG_IOCTL_NR_MAX; nr++) {
        uint64 ticks = hvdxg.ioctl_nr_total_ticks[nr];
        if (hvdxg.ioctl_nr_count[nr] == 0)
            continue;
        for (uint32 slot = 0; slot < HV_DXG_IOCTL_TIME_TOP; slot++) {
            if (ticks <= top_ticks[slot])
                continue;
            for (uint32 move = HV_DXG_IOCTL_TIME_TOP - 1; move > slot;
                 move--) {
                top_ticks[move] = top_ticks[move - 1];
                top_nr[move] = top_nr[move - 1];
            }
            top_ticks[slot] = ticks;
            top_nr[slot] = nr;
            break;
        }
    }
}

static void hvdxg_save_priv_head(uint8 *dst, uint32 dst_cap, uint32 *dst_len,
                                 const uint8 *src, uint32 src_len)
{
    uint32 len = src_len < dst_cap ? src_len : dst_cap;

    *dst_len = len;
    memset(dst, 0, dst_cap);
    if (len != 0)
        memcpy(dst, src, len);
}

static uint32 hvdxg_runtime_d3d12_resource_flags(const uint8 *runtime,
                                                 uint32 runtime_size)
{
    uint32 flags = 0;

    /*
     * The observed WSL/NVIDIA D3D12 runtime blob carries the
     * D3D12_RESOURCE_DESC flags at this offset.  The raw blob is still exposed
     * in /dev/dxg; this extracted value lets the NT-export guard distinguish a
     * render target from a copied shared texture.
     */
    if (runtime != NULL && runtime_size >= 0xf0)
        memcpy(&flags, runtime + 0xec, sizeof(flags));
    return flags;
}

static uint16 hvdxg_read_u16_at(const uint8 *data, uint32 size, uint32 off)
{
    uint16 value = 0;

    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(&value, data + off, sizeof(value));
    return value;
}

static uint32 hvdxg_read_u32_at(const uint8 *data, uint32 size, uint32 off)
{
    uint32 value = 0;

    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(&value, data + off, sizeof(value));
    return value;
}

static uint64 hvdxg_read_u64_at(const uint8 *data, uint32 size, uint32 off)
{
    uint64 value = 0;

    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(&value, data + off, sizeof(value));
    return value;
}

static void hvdxg_write_u32_at(uint8 *data, uint32 size, uint32 off,
                               uint32 value)
{
    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(data + off, &value, sizeof(value));
}

static void hvdxg_write_u64_at(uint8 *data, uint32 size, uint32 off,
                               uint64 value)
{
    if (data != NULL && off <= size && size - off >= sizeof(value))
        memcpy(data + off, &value, sizeof(value));
}

static uint32 hvdxg_hash_bytes(const uint8 *data, uint32 size)
{
    uint32 hash = 2166136261U;

    if (data == NULL)
        return 0;
    for (uint32 i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

static int hvdxg_queryadapter_alias_cache_type(uint32 type)
{
    return type == _KMTQAITYPE_UMDRIVERNAME ||
           type == _KMTQAITYPE_ADAPTERTYPE ||
           type == _KMTQAITYPE_PHYSICALADAPTERCOUNT ||
           type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID ||
           type == _KMTQAITYPE_ADAPTERTYPE_RENDER;
}

static struct hvdxg_queryadapter_alias_cache_entry *
hvdxg_queryadapter_alias_cache_find(uint32 type, uint32 host_adapter)
{
    if (!hvdxg_queryadapter_alias_cache_type(type) || host_adapter == 0)
        return NULL;
    for (uint32 i = 0; i < HV_DXG_QUERYADAPTER_ALIAS_CACHE_TYPES; i++) {
        struct hvdxg_queryadapter_alias_cache_entry *entry =
            &hvdxg.queryadapter_alias_cache[i];

        if (entry->valid && entry->type == type &&
            entry->host_adapter == host_adapter)
            return entry;
    }
    return NULL;
}

static int hvdxg_queryadapter_type0_cache_load(uint32 requested_size,
                                               uint32 alias_adapter,
                                               uint32 host_adapter,
                                               uint8 *out)
{
    hvdxg.queryadapter_alias_cache_last_type =
        HV_DXG_QAITYPE_SELECTED_ADAPTER;
    hvdxg.queryadapter_alias_cache_last_size = requested_size;
    hvdxg.queryadapter_alias_cache_last_len = 0;
    hvdxg.queryadapter_alias_cache_last_alias = alias_adapter;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash = 0;
    hvdxg.queryadapter_alias_cache_last_result = 0;

    if (!hvdxg.queryadapter_type0_cache_valid ||
        hvdxg.queryadapter_type0_cache_payload == NULL ||
        hvdxg.queryadapter_type0_cache_host != host_adapter ||
        hvdxg.queryadapter_type0_cache_input_hash !=
            hvdxg.queryadapter_type0_private_hash ||
        hvdxg.queryadapter_type0_cache_size < requested_size ||
        out == NULL) {
        hvdxg.queryadapter_alias_cache_misses++;
        hvdxg.queryadapter_alias_cache_last_result = 2;
        return -ENOENT;
    }

    memcpy(out, hvdxg.queryadapter_type0_cache_payload, requested_size);
    hvdxg.queryadapter_alias_cache_hits++;
    hvdxg.queryadapter_alias_cache_last_len = requested_size;
    hvdxg.queryadapter_alias_cache_last_hash =
        hvdxg.queryadapter_type0_cache_hash;
    hvdxg.queryadapter_alias_cache_last_result = 1;
    hvdxg_note_queryadapter_return_head(out, requested_size);
    return 0;
}

static void hvdxg_queryadapter_type0_cache_store(uint32 size,
                                                 uint32 host_adapter,
                                                 const uint8 *payload)
{
    uint8 *copy;

    if (host_adapter == 0 || payload == NULL || size == 0 ||
        size > HV_DXG_QUERYADAPTER_TYPE0_CACHE_MAX)
        return;

    copy = kvmalloc(size);
    if (copy == NULL) {
        hvdxg.queryadapter_alias_cache_full++;
        hvdxg.queryadapter_alias_cache_last_type =
            HV_DXG_QAITYPE_SELECTED_ADAPTER;
        hvdxg.queryadapter_alias_cache_last_size = size;
        hvdxg.queryadapter_alias_cache_last_len = 0;
        hvdxg.queryadapter_alias_cache_last_alias = 0;
        hvdxg.queryadapter_alias_cache_last_host = host_adapter;
        hvdxg.queryadapter_alias_cache_last_hash = 0;
        hvdxg.queryadapter_alias_cache_last_result = 5;
        return;
    }
    memcpy(copy, payload, size);
    if (hvdxg.queryadapter_type0_cache_payload != NULL)
        kvfree(hvdxg.queryadapter_type0_cache_payload);
    hvdxg.queryadapter_type0_cache_payload = copy;
    hvdxg.queryadapter_type0_cache_valid = 1;
    hvdxg.queryadapter_type0_cache_size = size;
    hvdxg.queryadapter_type0_cache_host = host_adapter;
    hvdxg.queryadapter_type0_cache_input_hash =
        hvdxg.queryadapter_type0_private_hash;
    hvdxg.queryadapter_type0_cache_hash = hvdxg_hash_bytes(payload, size);
    hvdxg.queryadapter_alias_cache_stores++;
    hvdxg.queryadapter_alias_cache_last_type =
        HV_DXG_QAITYPE_SELECTED_ADAPTER;
    hvdxg.queryadapter_alias_cache_last_size = size;
    hvdxg.queryadapter_alias_cache_last_len = size;
    hvdxg.queryadapter_alias_cache_last_alias = 0;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash =
        hvdxg.queryadapter_type0_cache_hash;
    hvdxg.queryadapter_alias_cache_last_result = 3;
}

static int hvdxg_queryadapter_alias_cache_load(uint32 type,
                                               uint32 requested_size,
                                               uint32 alias_adapter,
                                               uint32 host_adapter,
                                               uint8 *out)
{
    struct hvdxg_queryadapter_alias_cache_entry *entry;

    if (type == HV_DXG_QAITYPE_SELECTED_ADAPTER)
        return hvdxg_queryadapter_type0_cache_load(requested_size,
            alias_adapter, host_adapter, out);

    hvdxg.queryadapter_alias_cache_last_type = type;
    hvdxg.queryadapter_alias_cache_last_size = requested_size;
    hvdxg.queryadapter_alias_cache_last_len = 0;
    hvdxg.queryadapter_alias_cache_last_alias = alias_adapter;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash = 0;
    hvdxg.queryadapter_alias_cache_last_result = 0;

    entry = hvdxg_queryadapter_alias_cache_find(type, host_adapter);
    if (entry == NULL || entry->size < requested_size ||
        out == NULL || requested_size > HV_DXG_QUERYADAPTER_ALIAS_CACHE_MAX) {
        hvdxg.queryadapter_alias_cache_misses++;
        hvdxg.queryadapter_alias_cache_last_result = 2;
        return -ENOENT;
    }

    memcpy(out, entry->payload, requested_size);
    hvdxg.queryadapter_alias_cache_hits++;
    hvdxg.queryadapter_alias_cache_last_len = requested_size;
    hvdxg.queryadapter_alias_cache_last_hash =
        hvdxg_hash_bytes(entry->payload, entry->size);
    hvdxg.queryadapter_alias_cache_last_result = 1;
    hvdxg_note_queryadapter_return_head(entry->payload, requested_size);
    return 0;
}

static void hvdxg_queryadapter_alias_cache_store(uint32 type,
                                                 uint32 size,
                                                 uint32 host_adapter,
                                                 const uint8 *payload)
{
    struct hvdxg_queryadapter_alias_cache_entry *entry = NULL;

    if (type == HV_DXG_QAITYPE_SELECTED_ADAPTER) {
        hvdxg_queryadapter_type0_cache_store(size, host_adapter, payload);
        return;
    }

    if (!hvdxg_queryadapter_alias_cache_type(type) ||
        host_adapter == 0 || payload == NULL ||
        size == 0 || size > HV_DXG_QUERYADAPTER_ALIAS_CACHE_MAX)
        return;

    entry = hvdxg_queryadapter_alias_cache_find(type, host_adapter);
    if (entry == NULL) {
        for (uint32 i = 0; i < HV_DXG_QUERYADAPTER_ALIAS_CACHE_TYPES; i++) {
            if (!hvdxg.queryadapter_alias_cache[i].valid) {
                entry = &hvdxg.queryadapter_alias_cache[i];
                break;
            }
        }
    }
    if (entry == NULL) {
        hvdxg.queryadapter_alias_cache_full++;
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->valid = 1;
    entry->type = type;
    entry->size = size;
    entry->host_adapter = host_adapter;
    memcpy(entry->payload, payload, size);
    entry->raw_hash = hvdxg_hash_bytes(payload, size);
    hvdxg.queryadapter_alias_cache_stores++;
    hvdxg.queryadapter_alias_cache_last_type = type;
    hvdxg.queryadapter_alias_cache_last_size = size;
    hvdxg.queryadapter_alias_cache_last_len = size;
    hvdxg.queryadapter_alias_cache_last_alias = 0;
    hvdxg.queryadapter_alias_cache_last_host = host_adapter;
    hvdxg.queryadapter_alias_cache_last_hash = entry->raw_hash;
    hvdxg.queryadapter_alias_cache_last_result = 3;
}

static int hvdxg_queryadapter_stage_umd_payload(uint32 type, uint32 size,
                                                uint32 alias_adapter,
                                                uint32 host_adapter,
                                                uint8 *out)
{
    uint32 need;

    hvdxg.queryadapter_alias_staged_type = type;
    hvdxg.queryadapter_alias_staged_size = size;
    hvdxg.queryadapter_alias_staged_alias = alias_adapter;
    hvdxg.queryadapter_alias_staged_host = host_adapter;
    hvdxg.queryadapter_alias_staged_path = 0;
    hvdxg.queryadapter_alias_staged_ret = 0;

    if (type != _KMTQAITYPE_UMDRIVERNAME || out == NULL ||
        hvdxg.adapter_vendor_id != HV_DXG_VENDOR_NVIDIA) {
        hvdxg.queryadapter_alias_staged_ret = -ENOENT;
        return -ENOENT;
    }
    need = ((uint32)strlen(HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH) + 1U) *
           sizeof(uint16);
    if (size < need) {
        hvdxg.queryadapter_alias_staged_ret = -EOVERFLOW;
        return -EOVERFLOW;
    }

    memset(out, 0, size);
    hvdxg_write_utf16_string(out, size, HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH);
    hvdxg.queryadapter_alias_staged_path = 1;
    hvdxg_note_queryadapter_return_head(out, size);
    return 0;
}

static int hvdxg_rewrite_umd_driver_path(uint8 *data, uint32 size,
                                         uint32 host_status)
{
    const char *path = NULL;
    uint32 path_id = 0;
    uint32 need;

    hvdxg.queryadapter_umd_rewrite_attempted = 1;
    hvdxg.queryadapter_umd_rewrite_rewritten = 0;
    hvdxg.queryadapter_umd_rewrite_path = 0;
    hvdxg.queryadapter_umd_rewrite_vendor = hvdxg.adapter_vendor_id;
    hvdxg.queryadapter_umd_rewrite_original_hash =
        hvdxg_hash_bytes(data, size);
    hvdxg.queryadapter_umd_rewrite_original0 =
        size >= sizeof(uint32) ? hvdxg_read_u32(data) : 0;
    hvdxg.queryadapter_umd_rewrite_original1 =
        size >= 2 * sizeof(uint32) ?
        hvdxg_read_u32(data + sizeof(uint32)) : 0;
    hvdxg.queryadapter_umd_rewrite_host_status = host_status;
    hvdxg.queryadapter_umd_rewrite_ret = 0;

    if (hvdxg.adapter_vendor_id == HV_DXG_VENDOR_NVIDIA) {
        path = HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH;
        path_id = 1;
    } else if (hvdxg.adapter_vendor_id == HV_DXG_VENDOR_INTEL) {
        path = "/lib/libigd12umd64.so";
        path_id = 2;
    } else if (data != NULL) {
        uint32 chars = size / sizeof(uint16);
        uint16 *words = (uint16 *)data;

        for (uint32 i = 0; i + 1 < chars && path == NULL; i++) {
            uint16 c0 = words[i] >= 'A' && words[i] <= 'Z' ?
                        words[i] + ('a' - 'A') : words[i];
            uint16 c1 = words[i + 1] >= 'A' && words[i + 1] <= 'Z' ?
                        words[i + 1] + ('a' - 'A') : words[i + 1];

            if (c0 == 'n' && c1 == 'v') {
                path = HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH;
                path_id = 1;
                hvdxg.queryadapter_umd_rewrite_vendor =
                    HV_DXG_VENDOR_NVIDIA;
            } else if (i + 2 < chars && c0 == 'i' && c1 == 'g') {
                uint16 c2 = words[i + 2] >= 'A' &&
                            words[i + 2] <= 'Z' ?
                            words[i + 2] + ('a' - 'A') : words[i + 2];
                if (c2 == 'd') {
                    path = "/lib/libigd12umd64.so";
                    path_id = 2;
                    hvdxg.queryadapter_umd_rewrite_vendor =
                        HV_DXG_VENDOR_INTEL;
                }
            }
        }
    }
    if (path == NULL) {
        hvdxg.queryadapter_umd_rewrite_ret = -ENOTSUP;
        return 0;
    }

    need = ((uint32)strlen(path) + 1U) * sizeof(uint16);
    hvdxg.queryadapter_umd_rewrite_path = path_id;
    if (size < need) {
        hvdxg.queryadapter_umd_rewrite_ret = -EOVERFLOW;
        return -EOVERFLOW;
    }
    memset(data, 0, size);
    hvdxg_write_utf16_string(data, size, path);
    hvdxg.queryadapter_umd_rewrite_rewritten = 1;
    return 0;
}

static void hvdxg_note_queryadapter_type0_input(const uint8 *data,
                                                uint32 size)
{
    uint32 head_len = size < HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES ?
                      size : HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES;
    uint32 tail_len = head_len;

    hvdxg.queryadapter_type0_private_size = size;
    hvdxg.queryadapter_type0_private_hash = hvdxg_hash_bytes(data, size);
    hvdxg.queryadapter_type0_private_head_len = head_len;
    hvdxg.queryadapter_type0_private_tail_len = tail_len;
    memset(hvdxg.queryadapter_type0_private_head, 0,
           sizeof(hvdxg.queryadapter_type0_private_head));
    memset(hvdxg.queryadapter_type0_private_tail, 0,
           sizeof(hvdxg.queryadapter_type0_private_tail));
    if (data != NULL && head_len != 0)
        memcpy(hvdxg.queryadapter_type0_private_head, data, head_len);
    if (data != NULL && tail_len != 0)
        memcpy(hvdxg.queryadapter_type0_private_tail,
               data + size - tail_len, tail_len);

    hvdxg.queryadapter_type0_primary_len = 0;
    hvdxg.queryadapter_type0_primary_ret = 0;
    hvdxg.queryadapter_type0_primary_status = 0;
    hvdxg.queryadapter_type0_fallback_attempted = 0;
    hvdxg.queryadapter_type0_fallback_used = 0;
    hvdxg.queryadapter_type0_fallback_len = 0;
    hvdxg.queryadapter_type0_fallback_ret = 0;
    hvdxg.queryadapter_type0_fallback_status = 0;
    hvdxg.queryadapter_type0_result_route = 0;
    hvdxg.queryadapter_type0_fallback_reason = 0;
    hvdxg.queryadapter_type0_fallback_route = 0;
    hvdxg.queryadapter_umd_rewrite_attempted = 0;
    hvdxg.queryadapter_umd_rewrite_rewritten = 0;
    hvdxg.queryadapter_umd_rewrite_path = 0;
    hvdxg.queryadapter_umd_rewrite_vendor = 0;
    hvdxg.queryadapter_umd_rewrite_original_hash = 0;
    hvdxg.queryadapter_umd_rewrite_original0 = 0;
    hvdxg.queryadapter_umd_rewrite_original1 = 0;
    hvdxg.queryadapter_umd_rewrite_host_status = 0;
    hvdxg.queryadapter_umd_rewrite_ret = 0;
}

static void hvdxg_note_queryadapter_type15_failure(uint32 type, int ret,
                                                   int32 status,
                                                   uint32 process)
{
    if (type != 15U || ret == 0)
        return;
    hvdxg.queryadapter_type15_fail_ret = ret;
    hvdxg.queryadapter_type15_fail_status = status;
    hvdxg.queryadapter_type15_fail_process = process;
    hvdxg.queryadapter_type15_fail_route = hvdxg.queryadapter_send_route;
    hvdxg.queryadapter_type15_fail_ext_luid_low =
        hvdxg.queryadapter_send_ext_luid_low;
    hvdxg.queryadapter_type15_fail_ext_luid_high =
        hvdxg.queryadapter_send_ext_luid_high;
}

static void hvdxg_note_shared_resource_metadata(
    const struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL) {
        hvdxg.sharedresource_meta_track_host = 0;
        hvdxg.sharedresource_meta_runtime_len = 0;
        hvdxg.sharedresource_meta_resource_len = 0;
        hvdxg.sharedresource_meta_total_len = 0;
        hvdxg.sharedresource_meta_alloc0_priv = 0;
        hvdxg.sharedresource_meta_runtime_hash = 0;
        hvdxg.sharedresource_meta_resource_hash = 0;
        hvdxg.sharedresource_meta_total_hash = 0;
        hvdxg.sharedresource_meta_match_in = 0;
        hvdxg.sharedresource_meta_match_out = 0;
        hvdxg.sharedresource_meta_total_w4 = 0;
        hvdxg.sharedresource_meta_total_w8 = 0;
        hvdxg.sharedresource_meta_logical_flags = 0;
        hvdxg.sharedresource_meta_host_result_flags = 0;
        hvdxg.sharedresource_meta_host_flags_ignored = 0;
        return;
    }
    hvdxg.sharedresource_meta_track_host = resource->total_priv_from_host;
    hvdxg.sharedresource_meta_runtime_len =
        resource->private_runtime_data_size;
    hvdxg.sharedresource_meta_resource_len =
        resource->resource_priv_drv_data_size;
    hvdxg.sharedresource_meta_total_len =
        resource->total_priv_drv_data_size;
    hvdxg.sharedresource_meta_alloc0_priv =
        resource->allocation_count != 0 ? resource->alloc_priv_sizes[0] : 0;
    hvdxg.sharedresource_meta_runtime_hash =
        hvdxg_hash_bytes(resource->private_runtime_data,
                         resource->private_runtime_data_size);
    hvdxg.sharedresource_meta_resource_hash =
        hvdxg_hash_bytes(resource->resource_priv_drv_data,
                         resource->resource_priv_drv_data_size);
    hvdxg.sharedresource_meta_total_hash =
        hvdxg_hash_bytes(resource->total_priv_drv_data,
                         resource->total_priv_drv_data_size);
    hvdxg.sharedresource_meta_total_w4 =
        hvdxg_read_u32_at(resource->total_priv_drv_data,
                          resource->total_priv_drv_data_size, 0x10);
    hvdxg.sharedresource_meta_total_w8 =
        hvdxg_read_u32_at(resource->total_priv_drv_data,
                          resource->total_priv_drv_data_size, 0x20);
    hvdxg.sharedresource_meta_logical_flags = resource->create_flags_value;
    hvdxg.sharedresource_meta_host_result_flags =
        resource->host_create_flags_value;
    hvdxg.sharedresource_meta_host_flags_ignored =
        resource->host_create_flags_value != 0 &&
        resource->host_create_flags_value != resource->create_flags_value ?
        1 : 0;
    hvdxg.sharedresource_meta_match_in =
        resource->total_priv_drv_data_size ==
            hvdxg.d3d12_shared_alloc_priv_head_len &&
        resource->total_priv_drv_data_size != 0 &&
        memcmp(resource->total_priv_drv_data,
               hvdxg.d3d12_shared_alloc_priv_head,
               resource->total_priv_drv_data_size) == 0 ? 1 : 0;
    hvdxg.sharedresource_meta_match_out =
        resource->total_priv_drv_data_size ==
            hvdxg.d3d12_shared_alloc_out_priv_head_len &&
        resource->total_priv_drv_data_size != 0 &&
        memcmp(resource->total_priv_drv_data,
               hvdxg.d3d12_shared_alloc_out_priv_head,
               resource->total_priv_drv_data_size) == 0 ? 1 : 0;
}

static uint32 hvdxg_shared_resource_alloc0_hash(
    const struct hvdxg_tracked_resource *resource)
{
    if (resource == NULL || resource->allocation_count == 0 ||
        resource->alloc_priv_sizes[0] == 0 ||
        resource->total_priv_drv_data == NULL)
        return 0;
    if (resource->alloc_priv_sizes[0] > resource->total_priv_drv_data_size)
        return 0;
    return hvdxg_hash_bytes(resource->total_priv_drv_data,
                            resource->alloc_priv_sizes[0]);
}

static uint32 hvdxg_ntshared_cache_refs(uint32 kind, uint32 process,
                                        uint32 object,
                                        uint32 host_nt_handle);

static void hvdxg_reset_shared_resource_record(void)
{
    hvdxg.sharedresource_record_valid = 0;
    hvdxg.sharedresource_record_stage = 0;
    hvdxg.sharedresource_record_key_kind = 0;
    hvdxg.sharedresource_record_key_process = 0;
    hvdxg.sharedresource_record_key_object = 0;
    hvdxg.sharedresource_record_key_global = 0;
    hvdxg.sharedresource_record_key_nt = 0;
    hvdxg.sharedresource_record_source_process = 0;
    hvdxg.sharedresource_record_source_tgid = 0;
    hvdxg.sharedresource_record_source_generation = 0;
    hvdxg.sharedresource_record_device = 0;
    hvdxg.sharedresource_record_resource = 0;
    hvdxg.sharedresource_record_allocation = 0;
    hvdxg.sharedresource_record_adapter_low = 0;
    hvdxg.sharedresource_record_adapter_high = 0;
    hvdxg.sharedresource_record_host_adapter_low = 0;
    hvdxg.sharedresource_record_host_adapter_high = 0;
    hvdxg.sharedresource_record_sealed_generation = 0;
    hvdxg.sharedresource_record_sealed = 0;
    hvdxg.sharedresource_record_seal_before_fd = 0;
    hvdxg.sharedresource_record_alloc_count = 0;
    hvdxg.sharedresource_record_runtime_size = 0;
    hvdxg.sharedresource_record_resource_size = 0;
    hvdxg.sharedresource_record_total_size = 0;
    hvdxg.sharedresource_record_alloc0_priv = 0;
    hvdxg.sharedresource_record_runtime_hash = 0;
    hvdxg.sharedresource_record_resource_hash = 0;
    hvdxg.sharedresource_record_total_hash = 0;
    hvdxg.sharedresource_record_alloc0_hash = 0;
    hvdxg.sharedresource_record_nt_refs = 0;
    hvdxg.sharedresource_record_query_count = 0;
    hvdxg.sharedresource_record_open_count = 0;
    hvdxg.sharedresource_record_fd_publish_count = 0;
    hvdxg.sharedresource_record_local_admit_ret = 0;
    hvdxg.sharedresource_record_local_exact = 0;
    hvdxg.sharedresource_record_mutation_after_seal = 0;
}

static void hvdxg_note_shared_resource_record(
    const char *stage_name, uint32 stage,
    const struct hvdxg_tracked_resource *resource,
    uint32 kind, uint32 process, uint32 cache_object, uint32 global_share,
    uint32 host_nt_handle, int32 local_admit_ret, uint32 local_exact)
{
    uint32 runtime_hash = 0;
    uint32 resource_hash = 0;
    uint32 total_hash = 0;
    uint32 alloc0_hash = 0;
    uint64 source_tgid = current != NULL ? (uint64)thread_tgid(current) : 0;
    uint32 mutation = 0;

    (void)stage_name;
    if (resource == NULL)
        return;

    runtime_hash = hvdxg_hash_bytes(resource->private_runtime_data,
                                    resource->private_runtime_data_size);
    resource_hash = hvdxg_hash_bytes(resource->resource_priv_drv_data,
                                     resource->resource_priv_drv_data_size);
    total_hash = hvdxg_hash_bytes(resource->total_priv_drv_data,
                                  resource->total_priv_drv_data_size);
    alloc0_hash = hvdxg_shared_resource_alloc0_hash(resource);
    if (hvdxg.sharedresource_record_valid &&
        hvdxg.sharedresource_record_sealed &&
        hvdxg.sharedresource_record_key_process == process &&
        hvdxg.sharedresource_record_key_object == cache_object &&
        (hvdxg.sharedresource_record_runtime_hash != runtime_hash ||
         hvdxg.sharedresource_record_resource_hash != resource_hash ||
         hvdxg.sharedresource_record_total_hash != total_hash ||
         hvdxg.sharedresource_record_alloc0_hash != alloc0_hash))
        mutation = 1;
    if (hvdxg.sharedresource_record_valid &&
        hvdxg.sharedresource_record_key_process == process &&
        hvdxg.sharedresource_record_key_object == cache_object &&
        hvdxg.sharedresource_record_source_tgid != 0)
        source_tgid = hvdxg.sharedresource_record_source_tgid;

    hvdxg.sharedresource_record_valid = 1;
    hvdxg.sharedresource_record_stage = stage;
    hvdxg.sharedresource_record_key_kind = kind;
    hvdxg.sharedresource_record_key_process = process;
    hvdxg.sharedresource_record_key_object = cache_object;
    hvdxg.sharedresource_record_key_global = global_share;
    hvdxg.sharedresource_record_key_nt = host_nt_handle;
    hvdxg.sharedresource_record_source_process = resource->owner_process;
    hvdxg.sharedresource_record_source_tgid = source_tgid;
    hvdxg.sharedresource_record_source_generation =
        resource->owner_generation;
    hvdxg.sharedresource_record_device = resource->device;
    hvdxg.sharedresource_record_resource = resource->resource;
    hvdxg.sharedresource_record_allocation =
        resource->allocation_count != 0 ? resource->allocation_handles[0] : 0;
    hvdxg.sharedresource_record_adapter_low = hvdxg.adapter_luid.a;
    hvdxg.sharedresource_record_adapter_high = hvdxg.adapter_luid.b;
    hvdxg.sharedresource_record_host_adapter_low = hvdxg.host_adapter_luid.a;
    hvdxg.sharedresource_record_host_adapter_high = hvdxg.host_adapter_luid.b;
    hvdxg.sharedresource_record_sealed_generation =
        resource->sealed_generation;
    hvdxg.sharedresource_record_sealed = resource->sealed;
    hvdxg.sharedresource_record_alloc_count = resource->allocation_count;
    hvdxg.sharedresource_record_runtime_size =
        resource->private_runtime_data_size;
    hvdxg.sharedresource_record_resource_size =
        resource->resource_priv_drv_data_size;
    hvdxg.sharedresource_record_total_size =
        resource->total_priv_drv_data_size;
    hvdxg.sharedresource_record_alloc0_priv =
        resource->allocation_count != 0 ? resource->alloc_priv_sizes[0] : 0;
    hvdxg.sharedresource_record_runtime_hash = runtime_hash;
    hvdxg.sharedresource_record_resource_hash = resource_hash;
    hvdxg.sharedresource_record_total_hash = total_hash;
    hvdxg.sharedresource_record_alloc0_hash = alloc0_hash;
    hvdxg.sharedresource_record_nt_refs =
        hvdxg_ntshared_cache_refs(kind, process, cache_object,
                                  host_nt_handle);
    if (hvdxg.sharedresource_record_nt_refs == 0)
        hvdxg.sharedresource_record_nt_refs = resource->host_shared_refs;
    hvdxg.sharedresource_record_local_admit_ret = local_admit_ret;
    hvdxg.sharedresource_record_local_exact = local_exact;
    if (mutation)
        hvdxg.sharedresource_record_mutation_after_seal = 1;
}

static int hv_cmdline_enabled(const char *key);

static void hvdxg_normalize_d3d12_shared_alloc_priv(
    const struct d3dkmt_createallocation *req,
    const struct d3dddi_allocationinfo2 *alloc_info, uint8 *private_base)
{
    uint8 *runtime;
    uint8 *alloc_priv;
    uint32 alloc_size;
    uint32 w4;
    uint32 w8;
    uint32 width;
    uint32 height;
    uint64 rt8;
    uint64 rt10;

    hvdxg.d3d12_shared_norm_seen = 0;
    hvdxg.d3d12_shared_norm_applied = 0;
    hvdxg.d3d12_shared_norm_reason = 0;
    hvdxg.d3d12_shared_norm_magic0 = 0;
    hvdxg.d3d12_shared_norm_magic3 = 0;
    hvdxg.d3d12_shared_norm_pre_w4 = 0;
    hvdxg.d3d12_shared_norm_post_w4 = 0;
    hvdxg.d3d12_shared_norm_pre_w8 = 0;
    hvdxg.d3d12_shared_norm_post_w8 = 0;
    hvdxg.d3d12_shared_norm_runtime_applied = 0;
    hvdxg.d3d12_shared_norm_width = 0;
    hvdxg.d3d12_shared_norm_height = 0;
    hvdxg.d3d12_shared_norm_pre_rt8 = 0;
    hvdxg.d3d12_shared_norm_post_rt8 = 0;
    hvdxg.d3d12_shared_norm_pre_rt10 = 0;
    hvdxg.d3d12_shared_norm_post_rt10 = 0;
    hvdxg.d3d12_shared_norm_pre_rt38 = 0;
    hvdxg.d3d12_shared_norm_post_rt38 = 0;
    hvdxg.d3d12_shared_norm_pre_rt50 = 0;
    hvdxg.d3d12_shared_norm_post_rt50 = 0;
    hvdxg.d3d12_shared_norm_pre_rt58 = 0;
    hvdxg.d3d12_shared_norm_post_rt58 = 0;

    if (req == NULL || alloc_info == NULL || private_base == NULL)
        return;
    if (req->flags.value != 0x47)
        return;
    hvdxg.d3d12_shared_norm_seen = 1;
    if (req->alloc_count != 1 || req->private_runtime_data_size != 264 ||
        req->priv_drv_data_size != 0 ||
        alloc_info[0].priv_drv_data_size != 594) {
        hvdxg.d3d12_shared_norm_reason = 1;
        return;
    }

    alloc_size = alloc_info[0].priv_drv_data_size;
    runtime = private_base;
    alloc_priv = private_base + req->private_runtime_data_size +
                 req->priv_drv_data_size;
    hvdxg.d3d12_shared_norm_magic0 =
        hvdxg_read_u32_at(alloc_priv, alloc_size, 0);
    hvdxg.d3d12_shared_norm_magic3 =
        hvdxg_read_u32_at(alloc_priv, alloc_size, 12);
    if (hvdxg.d3d12_shared_norm_magic0 != 0x4e564441U ||
        hvdxg.d3d12_shared_norm_magic3 != 0x4e564458U) {
        hvdxg.d3d12_shared_norm_reason = 2;
        return;
    }

    w4 = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x10);
    w8 = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x20);
    width = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x30);
    height = hvdxg_read_u32_at(alloc_priv, alloc_size, 0x34);
    hvdxg.d3d12_shared_norm_pre_w4 = w4;
    hvdxg.d3d12_shared_norm_pre_w8 = w8;
    hvdxg.d3d12_shared_norm_width = width;
    hvdxg.d3d12_shared_norm_height = height;
    hvdxg.d3d12_shared_norm_pre_rt8 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x8);
    hvdxg.d3d12_shared_norm_pre_rt10 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x10);
    hvdxg.d3d12_shared_norm_pre_rt38 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x38);
    hvdxg.d3d12_shared_norm_pre_rt50 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x50);
    hvdxg.d3d12_shared_norm_pre_rt58 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x58);
    if (!hv_cmdline_enabled("dxg_d3d12_private_normalize")) {
        hvdxg.d3d12_shared_norm_post_w4 = w4;
        hvdxg.d3d12_shared_norm_post_w8 = w8;
        hvdxg.d3d12_shared_norm_post_rt8 =
            hvdxg.d3d12_shared_norm_pre_rt8;
        hvdxg.d3d12_shared_norm_post_rt10 =
            hvdxg.d3d12_shared_norm_pre_rt10;
        hvdxg.d3d12_shared_norm_post_rt38 =
            hvdxg.d3d12_shared_norm_pre_rt38;
        hvdxg.d3d12_shared_norm_post_rt50 =
            hvdxg.d3d12_shared_norm_pre_rt50;
        hvdxg.d3d12_shared_norm_post_rt58 =
            hvdxg.d3d12_shared_norm_pre_rt58;
        hvdxg.d3d12_shared_norm_reason = 4;
        return;
    }
    /*
     * Preserve the NVIDIA runtime/private blobs and overlay only the scalar
     * fields under an explicit diagnostic boot flag.  The WSL parity path
     * forwards private bytes verbatim.
     */
    w4 |= 0x8U;
    w8 |= 0x1U;
    rt8 = ((uint64)height << 32) | width;
    rt10 = ((uint64)1U << 32) | 0x57U;
    hvdxg_write_u32_at(alloc_priv, alloc_size, 0x10, w4);
    hvdxg_write_u32_at(alloc_priv, alloc_size, 0x20, w8);
    hvdxg_write_u64_at(runtime, req->private_runtime_data_size, 0x8, rt8);
    hvdxg_write_u64_at(runtime, req->private_runtime_data_size, 0x10, rt10);
    hvdxg.d3d12_shared_norm_post_w4 = w4;
    hvdxg.d3d12_shared_norm_post_w8 = w8;
    hvdxg.d3d12_shared_norm_post_rt8 = rt8;
    hvdxg.d3d12_shared_norm_post_rt10 = rt10;
    hvdxg.d3d12_shared_norm_post_rt38 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x38);
    hvdxg.d3d12_shared_norm_post_rt50 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x50);
    hvdxg.d3d12_shared_norm_post_rt58 =
        hvdxg_read_u64_at(runtime, req->private_runtime_data_size, 0x58);
    hvdxg.d3d12_shared_norm_runtime_applied = 1;
    hvdxg.d3d12_shared_norm_applied = 1;
    hvdxg.d3d12_shared_norm_reason = 3;
}

static void hvdxg_capture_d3d12_runtime_user(
    const struct d3dkmt_createallocation *req)
{
    uint32 copy_len;

    hvdxg.d3d12_shared_runtime_user_copy_ret = 0;
    hvdxg.d3d12_shared_runtime_user_mismatch = 0;
    hvdxg.d3d12_shared_runtime_user_head_len = 0;
    memset(hvdxg.d3d12_shared_runtime_user_head, 0,
           sizeof(hvdxg.d3d12_shared_runtime_user_head));

    if (req == NULL || req->flags.value != 0x47)
        return;
    copy_len = req->private_runtime_data_size;
    if (copy_len > sizeof(hvdxg.d3d12_shared_runtime_user_head))
        copy_len = sizeof(hvdxg.d3d12_shared_runtime_user_head);
    if (copy_len == 0)
        return;
    if (req->private_runtime_data == 0) {
        hvdxg.d3d12_shared_runtime_user_copy_ret = -EINVAL;
        return;
    }
    if (either_copyin(hvdxg.d3d12_shared_runtime_user_head, 1,
                      req->private_runtime_data, copy_len) < 0) {
        hvdxg.d3d12_shared_runtime_user_copy_ret = -EFAULT;
        return;
    }
    hvdxg.d3d12_shared_runtime_user_head_len = copy_len;
}

static void hvdxg_note_d3d12_shared_createallocation_request(
    const struct d3dkmt_createallocation *req,
    const struct d3dddi_allocationinfo2 *alloc_info,
    const uint8 *private_base, uint64 dxg_process)
{
    const uint8 *runtime;
    const uint8 *resource_priv;
    const uint8 *alloc_priv;

    if (req == NULL || alloc_info == NULL || private_base == NULL ||
        req->flags.value != 0x47)
        return;

    runtime = private_base;
    resource_priv = runtime + req->private_runtime_data_size;
    alloc_priv = resource_priv + req->priv_drv_data_size;

    hvdxg.d3d12_shared_create_seq = ++hvdxg.d3d12_shared_event_seq;
    hvdxg.d3d12_shared_first_nt_seq = 0;
    hvdxg.destroyalloc_d3d12_match_count = 0;
    hvdxg.destroyalloc_d3d12_pending_match_count = 0;
    hvdxg.destroyalloc_d3d12_last_match = 0;
    hvdxg.destroyalloc_d3d12_last_seq = 0;
    hvdxg.destroyalloc_d3d12_first_seq = 0;
    hvdxg.destroyalloc_d3d12_first_context = 0;
    hvdxg.destroyalloc_d3d12_first_match = 0;
    hvdxg.destroyalloc_d3d12_first_pending = 0;
    hvdxg.destroyalloc_d3d12_first_before_nt = 0;
    hvdxg.destroyalloc_d3d12_last_before_nt = 0;
    hvdxg.destroyalloc_d3d12_context_mask = 0;
    hvdxg.d3d12_shared_alloc_seen++;
    hvdxg.d3d12_shared_alloc_len = 0;
    hvdxg.d3d12_shared_alloc_ret = 0;
    hvdxg.d3d12_shared_alloc_device = req->device.v;
    hvdxg.d3d12_shared_alloc_resource_in = req->resource.v;
    hvdxg.d3d12_shared_alloc_resource_out = 0;
    hvdxg.d3d12_shared_alloc_allocation = 0;
    hvdxg.d3d12_shared_alloc_count = req->alloc_count;
    hvdxg.d3d12_shared_alloc_flags = req->flags.value;
    hvdxg.d3d12_shared_runtime_d3d12_flags =
        hvdxg_runtime_d3d12_resource_flags(runtime,
                                           req->private_runtime_data_size);
    hvdxg.d3d12_shared_alloc_global_share = req->global_share.v;
    hvdxg.d3d12_shared_allocinfo_offset =
        hvdxg.allocation_last_allocinfo_offset;
    hvdxg.d3d12_shared_runtime_offset =
        hvdxg.allocation_last_private_offset;
    hvdxg.d3d12_shared_resource_priv_offset =
        hvdxg.d3d12_shared_runtime_offset +
        req->private_runtime_data_size;
    hvdxg.d3d12_shared_alloc_priv_offset =
        hvdxg.d3d12_shared_resource_priv_offset + req->priv_drv_data_size;
    hvdxg.d3d12_shared_wire_flags = req->flags.value;
    hvdxg.d3d12_shared_wire_make_resident = 0;
    hvdxg.d3d12_shared_wire_rt_resource =
        req->private_runtime_resource_handle;
    hvdxg.d3d12_shared_result_flags = 0;
    hvdxg.d3d12_shared_result_global_share = 0;
    hvdxg.d3d12_shared_result_vgpu_flags = 0;
    hvdxg.d3d12_shared_result_alloc_flags = 0;
    hvdxg.d3d12_shared_result_min_len = 0;
    hvdxg.d3d12_shared_result_len = 0;
    hvdxg.d3d12_shared_result_flags_offset =
        hvdxg.allocation_last_result_flags_offset;
    hvdxg.d3d12_shared_result_resource_offset =
        hvdxg.allocation_last_result_resource_offset;
    hvdxg.d3d12_shared_result_global_offset =
        hvdxg.allocation_last_result_global_offset;
    hvdxg.d3d12_shared_result_vgpu_offset =
        hvdxg.allocation_last_result_vgpu_offset;
    hvdxg.d3d12_shared_result_allocinfo_offset =
        hvdxg.allocation_last_result_allocinfo_offset;
    hvdxg.d3d12_shared_result_allocinfo_size =
        hvdxg.allocation_last_result_allocinfo_size;
    hvdxg.d3d12_shared_result_head_len = 0;
    hvdxg.d3d12_shared_result_flag_norm = 0;
    hvdxg.d3d12_shared_result_flag_norm_reason = 0;
    hvdxg.d3d12_shared_result_flag_candidate = 0;
    hvdxg.d3d12_shared_result_flag_delta = 0;
    hvdxg.d3d12_shared_track_shared = req->flags.create_shared;
    hvdxg.d3d12_shared_track_nt = req->flags.nt_security_sharing;
    hvdxg.d3d12_shared_track_metadata =
        req->flags.create_shared && req->flags.nt_security_sharing ? 1 : 0;
    hvdxg.d3d12_shared_track_sent_bytes = 0;
    hvdxg.d3d12_shared_track_alloc_from_host = 0;
    hvdxg.d3d12_shared_alloc_process = dxg_process;
    hvdxg.d3d12_shared_alloc_size = 0;
    hvdxg.d3d12_shared_alloc_rt_resource =
        req->private_runtime_resource_handle;
    hvdxg.d3d12_shared_runtime_size = req->private_runtime_data_size;
    hvdxg.d3d12_shared_resource_priv_size = req->priv_drv_data_size;
    hvdxg.d3d12_shared_alloc_priv_size = alloc_info[0].priv_drv_data_size;
    hvdxg.d3d12_shared_alloc_out_priv_size = 0;
    hvdxg.d3d12_shared_runtime_head_len = 0;
    hvdxg.d3d12_shared_resource_priv_head_len = 0;
    hvdxg.d3d12_shared_alloc_priv_head_len = 0;
    hvdxg.d3d12_shared_alloc_out_priv_head_len = 0;
    hvdxg_save_priv_head(hvdxg.d3d12_shared_runtime_head,
                         sizeof(hvdxg.d3d12_shared_runtime_head),
                         &hvdxg.d3d12_shared_runtime_head_len,
                         runtime, req->private_runtime_data_size);
    if (hvdxg.d3d12_shared_runtime_user_copy_ret == 0) {
        uint32 compare_len = hvdxg.d3d12_shared_runtime_head_len;

        if (compare_len > hvdxg.d3d12_shared_runtime_user_head_len)
            compare_len = hvdxg.d3d12_shared_runtime_user_head_len;
        if (hvdxg.d3d12_shared_runtime_user_head_len !=
                hvdxg.d3d12_shared_runtime_head_len ||
            (compare_len != 0 &&
             memcmp(hvdxg.d3d12_shared_runtime_user_head,
                    hvdxg.d3d12_shared_runtime_head, compare_len) != 0))
            hvdxg.d3d12_shared_runtime_user_mismatch = 1;
    }
    hvdxg_save_priv_head(hvdxg.d3d12_shared_resource_priv_head,
                         sizeof(hvdxg.d3d12_shared_resource_priv_head),
                         &hvdxg.d3d12_shared_resource_priv_head_len,
                         resource_priv, req->priv_drv_data_size);
    hvdxg_save_priv_head(hvdxg.d3d12_shared_alloc_priv_head,
                         sizeof(hvdxg.d3d12_shared_alloc_priv_head),
                         &hvdxg.d3d12_shared_alloc_priv_head_len,
                         alloc_priv, alloc_info[0].priv_drv_data_size);
    memset(hvdxg.d3d12_shared_alloc_out_priv_head, 0,
           sizeof(hvdxg.d3d12_shared_alloc_out_priv_head));
    memset(hvdxg.d3d12_shared_result_head, 0,
           sizeof(hvdxg.d3d12_shared_result_head));
}

static void hvdxg_note_d3d12_shared_createallocation_result(
    uint32 len, int32 ret, const struct d3dkmt_createallocation *req,
    uint32 requested_flags,
    const struct d3dddi_allocationinfo2 *alloc_info,
    const struct hvdxg_command_createallocation_return *result,
    const uint8 *alloc_private_data)
{
    uint32 out_size = 0;

    if (req == NULL || requested_flags != 0x47)
        return;

    hvdxg.d3d12_shared_alloc_len = len;
    hvdxg.d3d12_shared_alloc_ret = ret;
    hvdxg.d3d12_shared_alloc_resource_out = req->resource.v;
    hvdxg.d3d12_shared_alloc_global_share = req->global_share.v;
    if (alloc_info != NULL)
        hvdxg.d3d12_shared_alloc_allocation = alloc_info[0].allocation.v;
    if (result != NULL) {
        hvdxg.d3d12_shared_result_min_len =
            hvdxg.allocation_last_result_min_len;
        hvdxg.d3d12_shared_result_len = hvdxg.allocation_last_result_len;
        hvdxg.d3d12_shared_result_flags_offset =
            hvdxg.allocation_last_result_flags_offset;
        hvdxg.d3d12_shared_result_resource_offset =
            hvdxg.allocation_last_result_resource_offset;
        hvdxg.d3d12_shared_result_global_offset =
            hvdxg.allocation_last_result_global_offset;
        hvdxg.d3d12_shared_result_vgpu_offset =
            hvdxg.allocation_last_result_vgpu_offset;
        hvdxg.d3d12_shared_result_allocinfo_offset =
            hvdxg.allocation_last_result_allocinfo_offset;
        hvdxg.d3d12_shared_result_allocinfo_size =
            hvdxg.allocation_last_result_allocinfo_size;
        hvdxg_save_priv_head(hvdxg.d3d12_shared_result_head,
                             sizeof(hvdxg.d3d12_shared_result_head),
                             &hvdxg.d3d12_shared_result_head_len,
                             (const uint8 *)result, len);
        hvdxg.d3d12_shared_alloc_size =
            result->allocation_info[0].allocation_size;
        hvdxg.d3d12_shared_result_flags = result->flags.value;
        hvdxg.d3d12_shared_result_global_share = result->global_share.v;
        hvdxg.d3d12_shared_result_vgpu_flags = result->vgpu_flags;
        hvdxg.d3d12_shared_result_alloc_flags =
            result->allocation_info[0].allocation_flags;
        hvdxg.d3d12_shared_result_flag_delta =
            result->flags.value ^ requested_flags;
        /*
         * Keep host-returned flags byte-for-byte.  WSL traces give us a
         * candidate mask to compare, but no local rule here proves 0x4000 is
         * safe to strip before user copyout or later tracking.
         */
        hvdxg.d3d12_shared_result_flag_norm = 0;
        hvdxg.d3d12_shared_result_flag_norm_reason =
            (result->flags.value & 0x4000U) != 0 ? 1 : 0;
        hvdxg.d3d12_shared_result_flag_candidate =
            result->flags.value & ~0x4000U;
        out_size = result->allocation_info[0].priv_drv_data_size;
    }
    if (alloc_private_data != NULL && out_size != 0) {
        hvdxg.d3d12_shared_alloc_out_priv_size = out_size;
        hvdxg_save_priv_head(hvdxg.d3d12_shared_alloc_out_priv_head,
                             sizeof(hvdxg.d3d12_shared_alloc_out_priv_head),
                             &hvdxg.d3d12_shared_alloc_out_priv_head_len,
                             alloc_private_data, out_size);
    }
}

static void hvdxg_status_append_hex(char *status, size_t status_size,
                                    int *len, const char *prefix,
                                    const uint8 *data, uint32 data_len)
{
    uint32 shown = data_len < HV_DXG_SHARED_ALLOC_HEAD_MAX ?
                   data_len : HV_DXG_SHARED_ALLOC_HEAD_MAX;

    if (*len < 0 || (size_t)*len >= status_size)
        return;
    *len += snprintf(status + *len, status_size - (size_t)*len,
                     "%s_len:%u %s:", prefix, data_len, prefix);
    for (uint32 i = 0; i < shown && *len > 0 &&
         (size_t)*len < status_size; i++) {
        *len += snprintf(status + *len, status_size - (size_t)*len,
                         "%02x", data[i]);
    }
    if ((size_t)*len < status_size)
        *len += snprintf(status + *len, status_size - (size_t)*len, "\n");
}

static void hvdxg_status_append_hex_inline(char *status, size_t status_size,
                                           int *len, const uint8 *data,
                                           uint32 data_len)
{
    if (*len < 0 || (size_t)*len >= status_size)
        return;
    for (uint32 i = 0; i < data_len && (size_t)*len < status_size; i++) {
        *len += snprintf(status + *len, status_size - (size_t)*len,
                         "%02x", data[i]);
    }
}

static void hvdxg_status_append_queryadapter_payloads(char *status,
                                                      size_t status_size,
                                                      int *len)
{
    if (*len < 0 || (size_t)*len >= status_size)
        return;
	    *len += snprintf(status + *len, status_size - (size_t)*len,
	        "dxg_queryadapter_source=last:%u real_host:%u alias_cache:%u "
	        "fallback:%u staged:%u cache_hits:%u cache_misses:%u stores:%u full:%u "
	        "last:type:%u size:%u len:%u alias:0x%x host:0x%x hash:%08x "
	        "result:%u staged_last:type:%u size:%u alias:0x%x host:0x%x "
	        "path:%u ret:%d type0_cache:%u/%u/0x%x/%08x/%08x\n",
        hvdxg.queryadapter_source_last,
        hvdxg.queryadapter_source_real_host,
        hvdxg.queryadapter_source_alias_cache,
        hvdxg.queryadapter_source_fallback,
        hvdxg.queryadapter_source_staged,
        hvdxg.queryadapter_alias_cache_hits,
        hvdxg.queryadapter_alias_cache_misses,
        hvdxg.queryadapter_alias_cache_stores,
        hvdxg.queryadapter_alias_cache_full,
        hvdxg.queryadapter_alias_cache_last_type,
        hvdxg.queryadapter_alias_cache_last_size,
        hvdxg.queryadapter_alias_cache_last_len,
        hvdxg.queryadapter_alias_cache_last_alias,
        hvdxg.queryadapter_alias_cache_last_host,
        hvdxg.queryadapter_alias_cache_last_hash,
        hvdxg.queryadapter_alias_cache_last_result,
        hvdxg.queryadapter_alias_staged_type,
        hvdxg.queryadapter_alias_staged_size,
	        hvdxg.queryadapter_alias_staged_alias,
	        hvdxg.queryadapter_alias_staged_host,
	        hvdxg.queryadapter_alias_staged_path,
	        hvdxg.queryadapter_alias_staged_ret,
	        hvdxg.queryadapter_type0_cache_valid,
	        hvdxg.queryadapter_type0_cache_size,
	        hvdxg.queryadapter_type0_cache_host,
	        hvdxg.queryadapter_type0_cache_input_hash,
	        hvdxg.queryadapter_type0_cache_hash);
    if (*len < 0 || (size_t)*len >= status_size)
        return;
    *len += snprintf(status + *len, status_size - (size_t)*len,
        "dxg_queryadapter_payload_history=index:%u "
        "route:0=none,1=vgpu,2=global\n",
        hvdxg.queryadapter_history_index);
    for (uint32 i = 0; i < HV_DXG_QUERY_HISTORY_MAX &&
         *len >= 0 && (size_t)*len < status_size; i++) {
        *len += snprintf(status + *len, status_size - (size_t)*len,
            "dxg_queryadapter_payload_h%u=type:%u size:%u len:%u "
            "ret:%d status:0x%x route:%u adapter:0x%x host:0x%x "
            "head_len:%u head:",
            i, hvdxg.queryadapter_history_type[i],
            hvdxg.queryadapter_history_size[i],
            hvdxg.queryadapter_history_len[i],
            hvdxg.queryadapter_history_ret[i],
            hvdxg.queryadapter_history_status[i],
            hvdxg.queryadapter_history_route[i],
            hvdxg.queryadapter_history_adapter[i],
            hvdxg.queryadapter_history_host_adapter[i],
            hvdxg.queryadapter_history_head_len[i]);
        hvdxg_status_append_hex_inline(status, status_size, len,
            hvdxg.queryadapter_history_head[i],
            hvdxg.queryadapter_history_head_len[i]);
        if (*len >= 0 && (size_t)*len < status_size)
            *len += snprintf(status + *len,
                             status_size - (size_t)*len, "\n");
    }
}

static void hvdxg_status_append_ioctl_timing(char *status, size_t status_size,
                                             int *len, const uint32 *top_nr)
{
    uint64 avg_us[HV_DXG_IOCTL_TIME_TOP];
    uint64 max_us[HV_DXG_IOCTL_TIME_TOP];
    uint64 last_us[HV_DXG_IOCTL_TIME_TOP];
    uint32 count[HV_DXG_IOCTL_TIME_TOP];

    for (uint32 i = 0; i < HV_DXG_IOCTL_TIME_TOP; i++) {
        uint32 nr = top_nr[i];

        count[i] = hvdxg.ioctl_nr_count[nr];
        avg_us[i] = count[i] == 0 ? 0 :
            hvdxg_ticks_to_us(hvdxg.ioctl_nr_total_ticks[nr] / count[i]);
        max_us[i] = hvdxg_ticks_to_us(hvdxg.ioctl_nr_max_ticks[nr]);
        last_us[i] = hvdxg_ticks_to_us(hvdxg.ioctl_nr_last_ticks[nr]);
    }

    if (*len < 0 || (size_t)*len >= status_size)
        return;
    *len += snprintf(status + *len, status_size - (size_t)*len,
        "d3dkmt_ioctl_time=timebase:%lu "
        "top0:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu "
        "top1:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu "
        "top2:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu "
        "top3:nr:%u count:%u avg_us:%lu max_us:%lu last_us:%lu\n",
        __timebase_frequency,
        top_nr[0], count[0], avg_us[0], max_us[0], last_us[0],
