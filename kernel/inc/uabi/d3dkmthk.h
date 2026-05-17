#ifndef __UAPI_D3DKMTHK_H
#define __UAPI_D3DKMTHK_H

#include <types.h>

#ifndef _IOC_NRBITS
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOWR(type, nr, size) \
    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))
#endif

#define D3DKMT_ADAPTERS_MAX 64
#define D3DDDI_MAX_BROADCAST_CONTEXT 64
#define D3DDDI_MAX_OBJECT_WAITED_ON 32
#define D3DDDI_MAX_OBJECT_SIGNALED 32
#define D3DDDI_MAX_WRITTEN_PRIMARIES 16

struct d3dkmthandle {
    union {
        struct {
            uint32 instance : 6;
            uint32 index : 24;
            uint32 unique : 2;
        };
        uint32 v;
    };
};

struct ntstatus {
    union {
        struct {
            int code : 16;
            int facility : 13;
            int customer : 1;
            int severity : 2;
        };
        int v;
    };
};

struct winluid {
    uint32 a;
    uint32 b;
};

struct d3dkmt_adapterinfo {
    struct d3dkmthandle adapter_handle;
    struct winluid adapter_luid;
    uint32 num_sources;
    uint32 present_move_regions_preferred;
};

struct d3dddi_allocationlist {
    struct d3dkmthandle allocation;
    union {
        struct {
            uint32 write_operation : 1;
            uint32 do_not_retire_instance : 1;
            uint32 offer_priority : 3;
            uint32 reserved : 27;
        };
        uint32 value;
    };
};

struct d3dddi_patchlocationlist {
    uint32 allocation_index;
    union {
        struct {
            uint32 slot_id : 24;
            uint32 reserved : 8;
        };
        uint32 value;
    };
    uint32 driver_id;
    uint32 allocation_offset;
    uint32 patch_offset;
    uint32 split_offset;
};

struct d3dkmt_enumadapters2 {
    uint32 num_adapters;
    uint32 reserved;
    uint64 adapters;
};

union d3dkmt_enumadapters_filter {
    struct {
        uint64 include_compute_only : 1;
        uint64 include_display_only : 1;
        uint64 reserved : 62;
    };
    uint64 value;
};

struct d3dkmt_enumadapters3 {
    union d3dkmt_enumadapters_filter filter;
    uint32 adapter_count;
    uint32 reserved;
    uint64 adapters;
};

struct d3dkmt_closeadapter {
    struct d3dkmthandle adapter_handle;
};

struct d3dkmt_openadapterfromluid {
    struct winluid adapter_luid;
    struct d3dkmthandle adapter_handle;
};

struct d3dkmt_adaptertype {
    union {
        struct {
            uint32 render_supported : 1;
            uint32 display_supported : 1;
            uint32 software_device : 1;
            uint32 post_device : 1;
            uint32 hybrid_discrete : 1;
            uint32 hybrid_integrated : 1;
            uint32 indirect_display_device : 1;
            uint32 paravirtualized : 1;
            uint32 acg_supported : 1;
            uint32 support_set_timings_from_vidpn : 1;
            uint32 detachable : 1;
            uint32 compute_only : 1;
            uint32 prototype : 1;
            uint32 reserved : 19;
        };
        uint32 value;
    };
};

enum kmtqueryadapterinfotype {
    _KMTQAITYPE_UMDRIVERPRIVATE = 0,
    _KMTQAITYPE_UMDRIVERNAME = 1,
    _KMTQAITYPE_ADAPTERTYPE = 15,
    _KMTQAITYPE_PHYSICALADAPTERCOUNT = 30,
    _KMTQAITYPE_QUERYREGISTRY = 48,
    _KMTQAITYPE_ADAPTERTYPE_RENDER = 57,
    _KMTQAITYPE_DRIVER_DESCRIPTION = 65,
    _KMTQAITYPE_DRIVER_DESCRIPTION_RENDER = 66,
};

struct d3dkmt_queryadapterinfo {
    struct d3dkmthandle adapter;
    enum kmtqueryadapterinfotype type;
    uint64 private_data;
    uint32 private_data_size;
};

#define D3DDDI_QUERYREGISTRY_MAX_PATH 260

enum d3dddi_queryregistry_type {
    D3DDDI_QUERYREGISTRY_SERVICEKEY = 0,
    D3DDDI_QUERYREGISTRY_ADAPTERKEY = 1,
    D3DDDI_QUERYREGISTRY_DRIVERSTOREPATH = 2,
    D3DDDI_QUERYREGISTRY_DRIVERIMAGEPATH = 3,
    D3DDDI_QUERYREGISTRY_MAX = 4,
};

struct d3dddi_queryregistry_flags {
    union {
        struct {
            uint32 translate_path : 1;
            uint32 mutable_value : 1;
            uint32 reserved : 30;
        };
        uint32 value;
    };
};

enum d3dddi_queryregistry_status {
    D3DDDI_QUERYREGISTRY_STATUS_SUCCESS = 0,
    D3DDDI_QUERYREGISTRY_STATUS_BUFFER_OVERFLOW = 1,
    D3DDDI_QUERYREGISTRY_STATUS_FAIL = 2,
};

struct d3dddi_queryregistry_info {
    enum d3dddi_queryregistry_type query_type;
    struct d3dddi_queryregistry_flags query_flags;
    uint16 value_name[D3DDDI_QUERYREGISTRY_MAX_PATH];
    uint32 value_type;
    uint32 physical_adapter_index;
    uint32 output_value_size;
    enum d3dddi_queryregistry_status status;
    union {
        uint32 output_dword;
        uint64 output_qword;
        uint16 output_string[1];
        uint8 output_binary[1];
    };
};

#define D3DDDI_QUERYREGISTRY_OUTPUT_OFFSET \
    (sizeof(struct d3dddi_queryregistry_info) - sizeof(uint64))

struct dxgk_gpuclockdata_flags {
    union {
        struct {
            uint32 context_management_processor : 1;
            uint32 reserved : 31;
        };
        uint32 value;
    };
};

struct dxgk_gpuclockdata {
    uint64 gpu_frequency;
    uint64 gpu_clock_counter;
    uint64 cpu_clock_counter;
    struct dxgk_gpuclockdata_flags flags;
} __PACKED;

struct d3dkmt_queryclockcalibration {
    struct d3dkmthandle adapter;
    uint32 node_ordinal;
    uint32 physical_adapter_index;
    struct dxgk_gpuclockdata clock_data;
} __PACKED;

struct d3dkmt_createdeviceflags {
    uint32 legacy_mode : 1;
    uint32 request_vSync : 1;
    uint32 disable_gpu_timeout : 1;
    uint32 gdi_device : 1;
    uint32 reserved : 28;
};

struct d3dkmt_createdevice {
    struct d3dkmthandle adapter;
    uint32 reserved3;
    struct d3dkmt_createdeviceflags flags;
    struct d3dkmthandle device;
    uint64 command_buffer;
    uint32 command_buffer_size;
    uint32 reserved;
    uint64 allocation_list;
    uint32 allocation_list_size;
    uint32 reserved1;
    uint64 patch_location_list;
    uint32 patch_location_list_size;
    uint32 reserved2;
};

struct d3dkmt_destroydevice {
    struct d3dkmthandle device;
};

enum dxgk_render_pipeline_stage {
    _DXGK_RENDER_PIPELINE_STAGE_UNKNOWN = 0,
    _DXGK_RENDER_PIPELINE_STAGE_INPUT_ASSEMBLER = 1,
    _DXGK_RENDER_PIPELINE_STAGE_VERTEX_SHADER = 2,
    _DXGK_RENDER_PIPELINE_STAGE_GEOMETRY_SHADER = 3,
    _DXGK_RENDER_PIPELINE_STAGE_STREAM_OUTPUT = 4,
    _DXGK_RENDER_PIPELINE_STAGE_RASTERIZER = 5,
    _DXGK_RENDER_PIPELINE_STAGE_PIXEL_SHADER = 6,
    _DXGK_RENDER_PIPELINE_STAGE_OUTPUT_MERGER = 7,
};

enum dxgk_page_fault_flags {
    _DXGK_PAGE_FAULT_WRITE = 0x1,
    _DXGK_PAGE_FAULT_FENCE_INVALID = 0x2,
    _DXGK_PAGE_FAULT_ADAPTER_RESET_REQUIRED = 0x4,
    _DXGK_PAGE_FAULT_ENGINE_RESET_REQUIRED = 0x8,
    _DXGK_PAGE_FAULT_FATAL_HARDWARE_ERROR = 0x10,
    _DXGK_PAGE_FAULT_IOMMU = 0x20,
    _DXGK_PAGE_FAULT_HW_CONTEXT_VALID = 0x40,
    _DXGK_PAGE_FAULT_PROCESS_HANDLE_VALID = 0x80,
};

enum dxgk_general_error_code {
    _DXGK_GENERAL_ERROR_PAGE_FAULT = 0,
    _DXGK_GENERAL_ERROR_INVALID_INSTRUCTION = 1,
};

struct dxgk_fault_error_code {
    union {
        struct {
            uint32 is_device_specific_code : 1;
            enum dxgk_general_error_code general_error_code : 31;
        };
        struct {
            uint32 is_device_specific_code_reserved_bit : 1;
            uint32 device_specific_code : 31;
        };
    };
};

struct d3dkmt_devicereset_state {
    union {
        struct {
            uint32 desktop_switched : 1;
            uint32 reserved : 31;
        };
        uint32 value;
    };
};

struct d3dkmt_devicepagefault_state {
    uint64 faulted_primitive_api_sequence_number;
    enum dxgk_render_pipeline_stage faulted_pipeline_stage;
    uint32 faulted_bind_table_entry;
    enum dxgk_page_fault_flags page_fault_flags;
    struct dxgk_fault_error_code fault_error_code;
    uint64 faulted_virtual_address;
};

enum d3dkmt_deviceexecution_state {
    _D3DKMT_DEVICEEXECUTION_ACTIVE = 1,
    _D3DKMT_DEVICEEXECUTION_RESET = 2,
    _D3DKMT_DEVICEEXECUTION_HUNG = 3,
    _D3DKMT_DEVICEEXECUTION_STOPPED = 4,
    _D3DKMT_DEVICEEXECUTION_ERROR_OUTOFMEMORY = 5,
    _D3DKMT_DEVICEEXECUTION_ERROR_DMAFAULT = 6,
    _D3DKMT_DEVICEEXECUTION_ERROR_DMAPAGEFAULT = 7,
};

enum d3dkmt_devicestate_type {
    _D3DKMT_DEVICESTATE_EXECUTION = 1,
    _D3DKMT_DEVICESTATE_PRESENT = 2,
    _D3DKMT_DEVICESTATE_RESET = 3,
    _D3DKMT_DEVICESTATE_PRESENT_DWM = 4,
    _D3DKMT_DEVICESTATE_PAGE_FAULT = 5,
    _D3DKMT_DEVICESTATE_PRESENT_QUEUE = 6,
};

struct d3dkmt_getdevicestate {
    struct d3dkmthandle device;
    enum d3dkmt_devicestate_type state_type;
    union {
        enum d3dkmt_deviceexecution_state execution_state;
        struct d3dkmt_devicereset_state reset_state;
        struct d3dkmt_devicepagefault_state page_fault_state;
        char alignment[48];
    };
};

enum d3dddi_knownescapetype {
    _D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE = 0,
    _D3DDDI_DRIVERESCAPETYPE_TRANSLATERESOURCEHANDLE = 1,
    _D3DDDI_DRIVERESCAPETYPE_CPUEVENTUSAGE = 2,
    _D3DDDI_DRIVERESCAPETYPE_BUILDTESTCOMMANDBUFFER = 3,
};

enum d3dkmt_escapetype {
    _D3DKMT_ESCAPE_DRIVERPRIVATE = 0,
    _D3DKMT_ESCAPE_VIDMM = 1,
    _D3DKMT_ESCAPE_VIDSCH = 3,
    _D3DKMT_ESCAPE_DEVICE = 4,
    _D3DKMT_ESCAPE_DRT_TEST = 8,
};

struct d3dddi_escapeflags {
    union {
        struct {
            uint32 hardware_access : 1;
            uint32 device_status_query : 1;
            uint32 change_frame_latency : 1;
            uint32 no_adapter_synchronization : 1;
            uint32 reserved : 1;
            uint32 virtual_machine_data : 1;
            uint32 driver_known_escape : 1;
            uint32 driver_common_escape : 1;
            uint32 reserved2 : 24;
        };
        uint32 value;
    };
};

struct d3dkmt_escape {
    struct d3dkmthandle adapter;
    struct d3dkmthandle device;
    enum d3dkmt_escapetype type;
    struct d3dddi_escapeflags flags;
    uint64 priv_drv_data;
    uint32 priv_drv_data_size;
    struct d3dkmthandle context;
};

enum d3dkmt_clienthint {
    _D3DKMT_CLIENTHNT_UNKNOWN = 0,
    _D3DKMT_CLIENTHINT_OPENGL = 1,
    _D3DKMT_CLIENTHINT_CDD = 2,
    _D3DKMT_CLIENTHINT_DX7 = 7,
    _D3DKMT_CLIENTHINT_DX8 = 8,
    _D3DKMT_CLIENTHINT_DX9 = 9,
    _D3DKMT_CLIENTHINT_DX10 = 10,
    _D3DKMT_CLIENTHINT_DX12 = 16,
};

struct d3dddi_createcontextflags {
    union {
        struct {
            uint32 null_rendering : 1;
            uint32 initial_data : 1;
            uint32 disable_gpu_timeout : 1;
            uint32 synchronization_only : 1;
            uint32 hw_queue_supported : 1;
            uint32 reserved : 27;
        };
        uint32 value;
    };
};

struct d3dddi_createhwqueueflags {
    union {
        struct {
            uint32 disable_gpu_timeout : 1;
            uint32 reserved : 31;
        };
        uint32 value;
    };
};

struct d3dkmt_destroycontext {
    struct d3dkmthandle context;
};

enum d3dkmdt_gdisurfacetype {
    _D3DKMDT_GDISURFACE_INVALID = 0,
    _D3DKMDT_GDISURFACE_TEXTURE = 1,
    _D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE = 2,
    _D3DKMDT_GDISURFACE_STAGING = 3,
    _D3DKMDT_GDISURFACE_LOOKUPTABLE = 4,
    _D3DKMDT_GDISURFACE_EXISTINGSYSMEM = 5,
    _D3DKMDT_GDISURFACE_TEXTURE_CPUVISIBLE = 6,
    _D3DKMDT_GDISURFACE_TEXTURE_CROSSADAPTER = 7,
    _D3DKMDT_GDISURFACE_TEXTURE_CPUVISIBLE_CROSSADAPTER = 8,
};

enum d3dddiformat {
    _D3DDDIFMT_UNKNOWN = 0,
};

struct d3dkmdt_gdisurfacedata {
    uint32 width;
    uint32 height;
    uint32 format;
    enum d3dkmdt_gdisurfacetype type;
    uint32 flags;
    uint32 pitch;
};

enum d3dkmdt_standardallocationtype {
    _D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE = 1,
    _D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE = 2,
    _D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE = 3,
    _D3DKMDT_STANDARDALLOCATION_GDISURFACE = 4,
};

enum d3dkmt_standardallocationtype {
    _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP = 1,
    _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER = 2,
};

struct d3dkmt_standardallocation_existingheap {
    uint64 size;
};

struct d3dkmt_createstandardallocationflags {
    union {
        struct {
            uint32 reserved : 32;
        };
        uint32 value;
    };
};

struct d3dkmt_createstandardallocation {
    enum d3dkmt_standardallocationtype type;
    uint32 reserved;
    struct d3dkmt_standardallocation_existingheap existing_heap_data;
    struct d3dkmt_createstandardallocationflags flags;
    uint32 reserved1;
};

struct d3dddi_allocationinfo2 {
    struct d3dkmthandle allocation;
    uint64 sysmem;
    uint64 priv_drv_data;
    uint32 priv_drv_data_size;
    uint32 vidpn_source_id;
    union {
        struct {
            uint32 primary : 1;
            uint32 stereo : 1;
            uint32 override_priority : 1;
            uint32 reserved : 29;
        };
        uint32 value;
    } flags;
    uint64 gpu_virtual_address;
    union {
        uint32 priority;
        uint64 unused;
    };
    uint64 reserved[5];
};

struct d3dkmt_createallocationflags {
    union {
        struct {
            uint32 create_resource : 1;
            uint32 create_shared : 1;
            uint32 non_secure : 1;
            uint32 create_protected : 1;
            uint32 restrict_shared_access : 1;
            uint32 existing_sysmem : 1;
            uint32 nt_security_sharing : 1;
            uint32 read_only : 1;
            uint32 create_write_combined : 1;
            uint32 create_cached : 1;
            uint32 swap_chain_back_buffer : 1;
            uint32 cross_adapter : 1;
            uint32 open_cross_adapter : 1;
            uint32 partial_shared_creation : 1;
            uint32 zeroed : 1;
            uint32 write_watch : 1;
            uint32 standard_allocation : 1;
            uint32 existing_section : 1;
            uint32 reserved : 14;
        };
        uint32 value;
    };
};

struct d3dkmt_createallocation {
    struct d3dkmthandle device;
    struct d3dkmthandle resource;
    struct d3dkmthandle global_share;
    uint32 reserved;
    uint64 private_runtime_data;
    uint32 private_runtime_data_size;
    uint32 reserved1;
    union {
        uint64 standard_allocation;
        uint64 priv_drv_data;
    };
    uint32 priv_drv_data_size;
    uint32 alloc_count;
    uint64 allocation_info;
    struct d3dkmt_createallocationflags flags;
    uint32 reserved2;
    uint64 private_runtime_resource_handle;
};

struct d3dddicb_destroyallocation2flags {
    union {
        struct {
            uint32 assume_not_in_use : 1;
            uint32 synchronous_destroy : 1;
            uint32 reserved : 29;
            uint32 system_use_only : 1;
        };
        uint32 value;
    };
};

struct d3dkmt_destroyallocation2 {
    struct d3dkmthandle device;
    struct d3dkmthandle resource;
    uint64 allocations;
    uint32 alloc_count;
    struct d3dddicb_destroyallocation2flags flags;
};

struct d3dkmt_createcontextvirtual {
    struct d3dkmthandle device;
    uint32 node_ordinal;
    uint32 engine_affinity;
    struct d3dddi_createcontextflags flags;
    uint64 priv_drv_data;
    uint32 priv_drv_data_size;
    enum d3dkmt_clienthint client_hint;
    struct d3dkmthandle context;
};

struct d3dkmt_createhwqueue {
    struct d3dkmthandle context;
    struct d3dddi_createhwqueueflags flags;
    uint32 priv_drv_data_size;
    uint32 reserved;
    uint64 priv_drv_data;
    struct d3dkmthandle queue;
    struct d3dkmthandle queue_progress_fence;
    uint64 queue_progress_fence_cpu_va;
    uint64 queue_progress_fence_gpu_va;
};

struct d3dkmt_destroyhwqueue {
    struct d3dkmthandle queue;
};

struct d3dddi_updateallocproperty_flags {
    union {
        struct {
            uint32 accessed_physically : 1;
            uint32 reserved : 31;
        };
        uint32 value;
    };
};

struct d3dddi_segmentpreference {
    union {
        struct {
            uint32 segment_id0 : 5;
            uint32 direction0 : 1;
            uint32 segment_id1 : 5;
            uint32 direction1 : 1;
            uint32 segment_id2 : 5;
            uint32 direction2 : 1;
            uint32 segment_id3 : 5;
            uint32 direction3 : 1;
            uint32 segment_id4 : 5;
            uint32 direction4 : 1;
            uint32 reserved : 2;
        };
        uint32 value;
    };
};

struct d3dddi_updateallocproperty {
    struct d3dkmthandle paging_queue;
    struct d3dkmthandle allocation;
    uint32 supported_segment_set;
    struct d3dddi_segmentpreference preferred_segment;
    struct d3dddi_updateallocproperty_flags flags;
    uint64 paging_fence_value;
    union {
        struct {
            uint32 set_accessed_physically : 1;
            uint32 set_supported_segmentset : 1;
            uint32 set_preferred_segment : 1;
            uint32 reserved : 29;
        };
        uint32 property_mask_value;
    };
};

struct d3dkmt_setcontextschedulingpriority {
    struct d3dkmthandle context;
    int priority;
};

struct d3dkmt_setcontextinprocessschedulingpriority {
    struct d3dkmthandle context;
    int priority;
};

struct d3dkmt_getcontextschedulingpriority {
    struct d3dkmthandle context;
    int priority;
};

struct d3dkmt_getcontextinprocessschedulingpriority {
    struct d3dkmthandle context;
    int priority;
};

struct d3dkmt_setallocationpriority {
    struct d3dkmthandle device;
    struct d3dkmthandle resource;
    uint64 allocation_list;
    uint32 allocation_count;
    uint32 reserved;
    uint64 priorities;
};

struct d3dkmt_getallocationpriority {
    struct d3dkmthandle device;
    struct d3dkmthandle resource;
    uint64 allocation_list;
    uint32 allocation_count;
    uint32 reserved;
    uint64 priorities;
};

enum d3dkmt_device_error_reason {
    _D3DKMT_DEVICE_ERROR_REASON_GENERIC = 0x80000000,
    _D3DKMT_DEVICE_ERROR_REASON_DRIVER_ERROR = 0x80000006,
};

struct d3dkmt_markdeviceaserror {
    struct d3dkmthandle device;
    enum d3dkmt_device_error_reason reason;
};

enum d3dkmt_allocationresidencystatus {
    _D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINGPUMEMORY = 1,
    _D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINSHAREDMEMORY = 2,
    _D3DKMT_ALLOCATIONRESIDENCYSTATUS_NOTRESIDENT = 3,
};

struct d3dkmt_queryallocationresidency {
    struct d3dkmthandle device;
    struct d3dkmthandle resource;
    uint64 allocations;
    uint32 allocation_count;
    uint32 reserved;
    uint64 residency_status;
};

enum d3dkmt_offer_priority {
    _D3DKMT_OFFER_PRIORITY_LOW = 1,
    _D3DKMT_OFFER_PRIORITY_NORMAL = 2,
    _D3DKMT_OFFER_PRIORITY_HIGH = 3,
    _D3DKMT_OFFER_PRIORITY_AUTO = 4,
};

struct d3dkmt_offer_flags {
    union {
        struct {
            uint32 offer_immediately : 1;
            uint32 allow_decommit : 1;
            uint32 reserved : 30;
        };
        uint32 value;
    };
};

struct d3dkmt_offerallocations {
    struct d3dkmthandle device;
    uint32 reserved;
    uint64 resources;
    uint64 allocations;
    uint32 allocation_count;
    enum d3dkmt_offer_priority priority;
    struct d3dkmt_offer_flags flags;
    uint32 reserved1;
};

enum d3dddi_reclaim_result {
    _D3DDDI_RECLAIM_RESULT_OK = 0,
    _D3DDDI_RECLAIM_RESULT_DISCARDED = 1,
    _D3DDDI_RECLAIM_RESULT_NOT_COMMITTED = 2,
};

struct d3dkmt_reclaimallocations2 {
    struct d3dkmthandle paging_queue;
    uint32 allocation_count;
    uint64 resources;
    uint64 allocations;
    union {
        uint64 discarded;
        uint64 results;
    };
    uint64 paging_fence_value;
};

struct d3dkmt_changevideomemoryreservation {
    uint64 process;
    struct d3dkmthandle adapter;
    uint32 memory_segment_group;
    uint64 reservation;
    uint32 physical_adapter_index;
};

struct d3dkmt_flushheaptransitions {
    struct d3dkmthandle adapter;
};

struct d3dkmt_invalidatecache {
    struct d3dkmthandle device;
    struct d3dkmthandle allocation;
    uint64 offset;
    uint64 length;
};

enum d3dddi_pagingqueue_priority {
    _D3DDDI_PAGINGQUEUE_PRIORITY_BELOW_NORMAL = -1,
    _D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL = 0,
    _D3DDDI_PAGINGQUEUE_PRIORITY_ABOVE_NORMAL = 1,
};

struct d3dddigpuva_protection_type {
    union {
        struct {
            uint64 write : 1;
            uint64 execute : 1;
            uint64 zero : 1;
            uint64 no_access : 1;
            uint64 system_use_only : 1;
            uint64 reserved : 59;
        };
        uint64 value;
    };
};

struct d3dddi_mapgpuvirtualaddress {
    struct d3dkmthandle paging_queue;
    uint64 base_address;
    uint64 minimum_address;
    uint64 maximum_address;
    struct d3dkmthandle allocation;
    uint64 offset_in_pages;
    uint64 size_in_pages;
    struct d3dddigpuva_protection_type protection;
    uint64 driver_protection;
    uint32 reserved0;
    uint64 reserved1;
    uint64 virtual_address;
    uint64 paging_fence_value;
};

enum d3dddigpuva_reservation_type {
    _D3DDDIGPUVA_RESERVE_NO_ACCESS = 0,
    _D3DDDIGPUVA_RESERVE_ZERO = 1,
    _D3DDDIGPUVA_RESERVE_NO_COMMIT = 2,
};

struct d3dddi_reservegpuvirtualaddress {
    struct d3dkmthandle adapter;
    uint64 base_address;
    uint64 minimum_address;
    uint64 maximum_address;
    uint64 size;
    enum d3dddigpuva_reservation_type reservation_type;
    uint64 driver_protection;
    uint64 virtual_address;
    uint64 paging_fence_value;
};

enum d3dddi_updategpuvirtualaddress_operation_type {
    _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP = 0,
    _D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP = 1,
    _D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY = 2,
    _D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT = 3,
};

struct d3dddi_updategpuvirtualaddress_operation {
    enum d3dddi_updategpuvirtualaddress_operation_type operation;
    union {
        struct {
            uint64 base_address;
            uint64 size;
            struct d3dkmthandle allocation;
            uint64 allocation_offset;
            uint64 allocation_size;
        } map;
        struct {
            uint64 base_address;
            uint64 size;
            struct d3dkmthandle allocation;
            uint64 allocation_offset;
            uint64 allocation_size;
            struct d3dddigpuva_protection_type protection;
            uint64 driver_protection;
        } map_protect;
        struct {
            uint64 base_address;
            uint64 size;
            struct d3dddigpuva_protection_type protection;
        } unmap;
        struct {
            uint64 source_address;
            uint64 size;
            uint64 dest_address;
        } copy;
    };
};

struct d3dkmt_updategpuvirtualaddressflags {
    union {
        struct {
            uint32 do_not_wait : 1;
            uint32 reserved : 31;
        };
        uint32 value;
    };
};

struct d3dkmt_updategpuvirtualaddress {
    struct d3dkmthandle device;
    struct d3dkmthandle context;
    struct d3dkmthandle fence_object;
    uint32 num_operations;
    uint64 operations;
    uint32 reserved0;
    uint32 reserved1;
    uint64 reserved2;
    uint64 fence_value;
    struct d3dkmt_updategpuvirtualaddressflags flags;
    uint32 reserved3;
};

struct d3dkmt_freegpuvirtualaddress {
    struct d3dkmthandle adapter;
    uint32 reserved;
    uint64 base_address;
    uint64 size;
};

struct d3dddicb_lock2flags {
    union {
        struct {
            uint32 reserved : 32;
        };
        uint32 value;
    };
};

struct d3dkmt_lock2 {
    struct d3dkmthandle device;
    struct d3dkmthandle allocation;
    struct d3dddicb_lock2flags flags;
    uint32 reserved;
    uint64 data;
};

struct d3dkmt_unlock2 {
    struct d3dkmthandle device;
    struct d3dkmthandle allocation;
};

struct d3dkmt_createpagingqueue {
    struct d3dkmthandle device;
    enum d3dddi_pagingqueue_priority priority;
    struct d3dkmthandle paging_queue;
    struct d3dkmthandle sync_object;
    uint64 fence_cpu_virtual_address;
    uint32 physical_adapter_index;
};

struct d3dddi_destroypagingqueue {
    struct d3dkmthandle paging_queue;
};

struct d3dddi_synchronizationobject_flags {
    union {
        struct {
            uint32 shared : 1;
            uint32 nt_security_sharing : 1;
            uint32 cross_adapter : 1;
            uint32 top_of_pipeline : 1;
            uint32 no_signal : 1;
            uint32 no_wait : 1;
            uint32 no_signal_max_value_on_tdr : 1;
            uint32 no_gpu_access : 1;
            uint32 reserved : 23;
        };
        uint32 value;
    };
};

enum d3dddi_synchronizationobject_type {
    _D3DDDI_SYNCHRONIZATION_MUTEX = 1,
    _D3DDDI_SEMAPHORE = 2,
    _D3DDDI_FENCE = 3,
    _D3DDDI_CPU_NOTIFICATION = 4,
    _D3DDDI_MONITORED_FENCE = 5,
    _D3DDDI_PERIODIC_MONITORED_FENCE = 6,
    _D3DDDI_SYNCHRONIZATION_TYPE_LIMIT,
};

struct d3dddi_synchronizationobjectinfo2 {
    enum d3dddi_synchronizationobject_type type;
    struct d3dddi_synchronizationobject_flags flags;
    union {
        struct {
            uint32 initial_state;
        } synchronization_mutex;
        struct {
            uint32 max_count;
            uint32 initial_count;
        } semaphore;
        struct {
            uint64 fence_value;
        } fence;
        struct {
            uint64 event;
        } cpu_notification;
        struct {
            uint64 initial_fence_value;
            uint64 fence_cpu_virtual_address;
            uint64 fence_gpu_virtual_address;
            uint32 engine_affinity;
        } monitored_fence;
        struct {
            struct d3dkmthandle adapter;
            uint32 vidpn_target_id;
            uint64 time;
            uint64 fence_cpu_virtual_address;
            uint64 fence_gpu_virtual_address;
            uint32 engine_affinity;
        } periodic_monitored_fence;
        struct {
            uint64 reserved[8];
        } reserved;
    };
    struct d3dkmthandle shared_handle;
};

struct d3dkmt_createsynchronizationobject2 {
    struct d3dkmthandle device;
    uint32 reserved;
    struct d3dddi_synchronizationobjectinfo2 info;
    struct d3dkmthandle sync_object;
    uint32 reserved1;
};

struct d3dddicb_signalflags {
    union {
        struct {
            uint32 signal_at_submission : 1;
            uint32 enqueue_cpu_event : 1;
            uint32 allow_fence_rewind : 1;
            uint32 reserved : 28;
            uint32 internal0 : 1;
        };
        uint32 value;
    };
};

struct d3dkmt_waitforsynchronizationobject2 {
    struct d3dkmthandle context;
    uint32 object_count;
    struct d3dkmthandle object_array[D3DDDI_MAX_OBJECT_WAITED_ON];
    union {
        struct {
            uint64 fence_value;
        } fence;
        uint64 reserved[8];
    };
};

struct d3dkmt_signalsynchronizationobject2 {
    struct d3dkmthandle context;
    uint32 object_count;
    struct d3dkmthandle object_array[D3DDDI_MAX_OBJECT_SIGNALED];
    struct d3dddicb_signalflags flags;
    uint32 context_count;
    struct d3dkmthandle contexts[D3DDDI_MAX_BROADCAST_CONTEXT];
    union {
        struct {
            uint64 fence_value;
        } fence;
        uint64 cpu_event_handle;
        uint64 reserved[8];
    };
};

struct d3dkmt_submitwaitforsyncobjectstohwqueue {
    struct d3dkmthandle hwqueue;
    uint32 object_count;
    uint64 objects;
    uint64 fence_values;
};

struct d3dkmt_submitsignalsyncobjectstohwqueue {
    struct d3dddicb_signalflags flags;
    uint32 hwqueue_count;
    uint64 hwqueues;
    uint32 object_count;
    uint32 reserved;
    uint64 objects;
    uint64 fence_values;
};

struct d3dddi_waitforsynchronizationobjectfromcpu_flags {
    union {
        struct {
            uint32 wait_any : 1;
            uint32 reserved : 31;
        };
        uint32 value;
    };
};

struct d3dkmt_signalsynchronizationobjectfromcpu {
    struct d3dkmthandle device;
    uint32 object_count;
    uint64 objects;
    uint64 fence_values;
    struct d3dddicb_signalflags flags;
};

struct d3dkmt_waitforsynchronizationobjectfromgpu {
    struct d3dkmthandle context;
    uint32 object_count;
    uint64 objects;
    union {
        uint64 monitored_fence_values;
        uint64 fence_value;
        uint64 reserved[8];
    };
};

struct d3dkmt_signalsynchronizationobjectfromgpu {
    struct d3dkmthandle context;
    uint32 object_count;
    uint64 objects;
    union {
        uint64 monitored_fence_values;
        uint64 reserved[8];
    };
};

struct d3dkmt_signalsynchronizationobjectfromgpu2 {
    uint32 object_count;
    uint32 reserved1;
    uint64 objects;
    struct d3dddicb_signalflags flags;
    uint32 context_count;
    uint64 contexts;
    union {
        uint64 fence_value;
        uint64 cpu_event_handle;
        uint64 monitored_fence_values;
        uint64 reserved[8];
    };
};

struct d3dkmt_waitforsynchronizationobjectfromcpu {
    struct d3dkmthandle device;
    uint32 object_count;
    uint64 objects;
    uint64 fence_values;
    uint64 async_event;
    struct d3dddi_waitforsynchronizationobjectfromcpu_flags flags;
};

struct d3dkmt_destroysynchronizationobject {
    struct d3dkmthandle sync_object;
};

struct d3dddi_makeresident_flags {
    union {
        struct {
            uint32 cant_trim_further : 1;
            uint32 must_succeed : 1;
            uint32 reserved : 30;
        };
        uint32 value;
    };
};

struct d3dddi_makeresident {
    struct d3dkmthandle paging_queue;
    uint32 alloc_count;
    uint64 allocation_list;
    uint64 priority_list;
    struct d3dddi_makeresident_flags flags;
    uint64 paging_fence_value;
    uint64 num_bytes_to_trim;
};

struct d3dddi_evict_flags {
    union {
        struct {
            uint32 evict_only_if_necessary : 1;
            uint32 not_written_to : 1;
            uint32 reserved : 30;
        };
        uint32 value;
    };
};

struct d3dkmt_evict {
    struct d3dkmthandle device;
    uint32 alloc_count;
    uint64 allocations;
    struct d3dddi_evict_flags flags;
    uint32 reserved;
    uint64 num_bytes_to_trim;
};

struct d3dkmt_submitcommandflags {
    union {
        struct {
            uint32 null_rendering : 1;
            uint32 present_redirected : 1;
            uint32 reserved : 30;
        };
        uint32 value;
    };
};

struct d3dkmt_submitcommand {
    uint64 command_buffer;
    uint32 command_length;
    struct d3dkmt_submitcommandflags flags;
    uint64 present_history_token;
    uint32 broadcast_context_count;
    struct d3dkmthandle broadcast_context[D3DDDI_MAX_BROADCAST_CONTEXT];
    uint32 reserved;
    uint64 priv_drv_data;
    uint32 priv_drv_data_size;
    uint32 num_primaries;
    struct d3dkmthandle written_primaries[D3DDDI_MAX_WRITTEN_PRIMARIES];
    uint32 num_history_buffers;
    uint32 reserved1;
    uint64 history_buffer_array;
};

struct d3dkmt_submitcommandtohwqueue {
    struct d3dkmthandle hwqueue;
    uint32 reserved;
    uint64 hwqueue_progress_fence_id;
    uint64 command_buffer;
    uint32 command_length;
    uint32 priv_drv_data_size;
    uint64 priv_drv_data;
    uint32 num_primaries;
    uint32 reserved1;
    uint64 written_primaries;
};

enum d3dkmt_memory_segment_group {
    _D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    _D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1,
};

struct d3dkmt_queryvideomemoryinfo {
    uint64 process;
    struct d3dkmthandle adapter;
    enum d3dkmt_memory_segment_group memory_segment_group;
    uint64 budget;
    uint64 current_usage;
    uint64 current_reservation;
    uint64 available_for_reservation;
    uint32 physical_adapter_index;
};

enum d3dkmt_querystatistics_type {
    _D3DKMT_QUERYSTATISTICS_ADAPTER = 0,
    _D3DKMT_QUERYSTATISTICS_PROCESS = 1,
    _D3DKMT_QUERYSTATISTICS_PROCESS_ADAPTER = 2,
    _D3DKMT_QUERYSTATISTICS_SEGMENT = 3,
    _D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT = 4,
    _D3DKMT_QUERYSTATISTICS_NODE = 5,
    _D3DKMT_QUERYSTATISTICS_PROCESS_NODE = 6,
    _D3DKMT_QUERYSTATISTICS_VIDPNSOURCE = 7,
    _D3DKMT_QUERYSTATISTICS_PROCESS_VIDPNSOURCE = 8,
    _D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT_GROUP = 9,
    _D3DKMT_QUERYSTATISTICS_PHYSICAL_ADAPTER = 10,
};

struct d3dkmt_querystatistics_result {
    char size[0x308];
};

struct d3dkmt_querystatistics {
    union {
        struct {
            enum d3dkmt_querystatistics_type type;
            struct winluid adapter_luid;
            uint64 process;
            struct d3dkmt_querystatistics_result result;
        };
        char size[0x328];
    };
};

struct d3dkmt_opensyncobjectfromnthandle2 {
    uint64 nt_handle;
    struct d3dkmthandle device;
    struct d3dddi_synchronizationobject_flags flags;
    struct d3dkmthandle sync_object;
    uint32 reserved1;
    union {
        struct {
            uint64 fence_value_cpu_va;
            uint64 fence_value_gpu_va;
            uint32 engine_affinity;
        } monitored_fence;
        uint64 reserved[8];
    };
};

struct d3dddi_openallocationinfo2 {
    struct d3dkmthandle allocation;
    uint64 priv_drv_data;
    uint32 priv_drv_data_size;
    uint64 gpu_va;
    uint64 reserved[6];
};

struct d3dkmt_openresourcefromnthandle {
    struct d3dkmthandle device;
    uint32 reserved;
    uint64 nt_handle;
    uint32 allocation_count;
    uint32 reserved1;
    uint64 open_alloc_info;
    int32 private_runtime_data_size;
    uint32 reserved2;
    uint64 private_runtime_data;
    uint32 resource_priv_drv_data_size;
    uint32 reserved3;
    uint64 resource_priv_drv_data;
    uint32 total_priv_drv_data_size;
    uint64 total_priv_drv_data;
    struct d3dkmthandle resource;
    struct d3dkmthandle keyed_mutex;
    uint64 keyed_mutex_private_data;
    uint32 keyed_mutex_private_data_size;
    struct d3dkmthandle sync_object;
};

struct d3dkmt_queryresourceinfofromnthandle {
    struct d3dkmthandle device;
    uint32 reserved;
    uint64 nt_handle;
    uint64 private_runtime_data;
    uint32 private_runtime_data_size;
    uint32 total_priv_drv_data_size;
    uint32 resource_priv_drv_data_size;
    uint32 allocation_count;
};

struct d3dkmt_shareobjects {
    uint32 object_count;
    uint32 reserved;
    uint64 objects;
    uint64 object_attr;
    uint32 desired_access;
    uint32 reserved1;
    uint64 shared_handle;
};

struct d3dkmt_shareobjectwithhost {
    struct d3dkmthandle device_handle;
    struct d3dkmthandle object_handle;
    uint64 reserved;
    uint64 object_vail_nt_handle;
};

struct d3dkmt_createsyncfile {
    struct d3dkmthandle device;
    struct d3dkmthandle monitored_fence;
    uint64 fence_value;
    uint64 sync_file_handle;
};

struct d3dkmt_waitsyncfile {
    uint64 sync_file_handle;
    struct d3dkmthandle context;
    uint32 reserved;
};

struct d3dkmt_opensyncobjectfromsyncfile {
    uint64 sync_file_handle;
    struct d3dkmthandle device;
    struct d3dkmthandle syncobj;
    uint64 fence_value;
    uint64 fence_value_cpu_va;
    uint64 fence_value_gpu_va;
};

struct d3dkmt_enumprocesses {
    struct winluid adapter_luid;
    uint64 buffer;
    uint64 buffer_count;
};

enum dxgk_feature_id {
    _DXGK_FEATURE_HWSCH = 0,
    _DXGK_FEATURE_PAGE_BASED_MEMORY_MANAGER = 32,
    _DXGK_FEATURE_KERNEL_MODE_TESTING = 33,
    _DXGK_FEATURE_MAX,
};

struct dxgk_isfeatureenabled_result {
    uint16 version;
    union {
        struct {
            uint16 enabled : 1;
            uint16 known_feature : 1;
            uint16 supported_by_driver : 1;
            uint16 supported_on_config : 1;
            uint16 reserved : 12;
        };
        uint16 value;
    };
};

struct d3dkmt_isfeatureenabled {
    struct d3dkmthandle adapter;
    enum dxgk_feature_id feature_id;
    struct dxgk_isfeatureenabled_result result;
};

#define LX_DXOPENADAPTERFROMLUID \
    _IOWR(0x47, 0x01, struct d3dkmt_openadapterfromluid)
#define LX_DXCREATEDEVICE \
    _IOWR(0x47, 0x02, struct d3dkmt_createdevice)
#define LX_DXCREATECONTEXTVIRTUAL \
    _IOWR(0x47, 0x04, struct d3dkmt_createcontextvirtual)
#define LX_DXDESTROYCONTEXT \
    _IOWR(0x47, 0x05, struct d3dkmt_destroycontext)
#define LX_DXCREATEALLOCATION \
    _IOWR(0x47, 0x06, struct d3dkmt_createallocation)
#define LX_DXCREATEPAGINGQUEUE \
    _IOWR(0x47, 0x07, struct d3dkmt_createpagingqueue)
#define LX_DXRESERVEGPUVIRTUALADDRESS \
    _IOWR(0x47, 0x08, struct d3dddi_reservegpuvirtualaddress)
#define LX_DXQUERYADAPTERINFO \
    _IOWR(0x47, 0x09, struct d3dkmt_queryadapterinfo)
#define LX_DXQUERYVIDEOMEMORYINFO \
    _IOWR(0x47, 0x0a, struct d3dkmt_queryvideomemoryinfo)
#define LX_DXMAKERESIDENT \
    _IOWR(0x47, 0x0b, struct d3dddi_makeresident)
#define LX_DXMAPGPUVIRTUALADDRESS \
    _IOWR(0x47, 0x0c, struct d3dddi_mapgpuvirtualaddress)
#define LX_DXESCAPE \
    _IOWR(0x47, 0x0d, struct d3dkmt_escape)
#define LX_DXGETDEVICESTATE \
    _IOWR(0x47, 0x0e, struct d3dkmt_getdevicestate)
#define LX_DXSUBMITCOMMAND \
    _IOWR(0x47, 0x0f, struct d3dkmt_submitcommand)
#define LX_DXCREATESYNCHRONIZATIONOBJECT \
    _IOWR(0x47, 0x10, struct d3dkmt_createsynchronizationobject2)
#define LX_DXSIGNALSYNCHRONIZATIONOBJECT \
    _IOWR(0x47, 0x11, struct d3dkmt_signalsynchronizationobject2)
#define LX_DXWAITFORSYNCHRONIZATIONOBJECT \
    _IOWR(0x47, 0x12, struct d3dkmt_waitforsynchronizationobject2)
#define LX_DXDESTROYALLOCATION2 \
    _IOWR(0x47, 0x13, struct d3dkmt_destroyallocation2)
#define LX_DXENUMADAPTERS2 \
    _IOWR(0x47, 0x14, struct d3dkmt_enumadapters2)
#define LX_DXCLOSEADAPTER \
    _IOWR(0x47, 0x15, struct d3dkmt_closeadapter)
#define LX_DXCHANGEVIDEOMEMORYRESERVATION \
    _IOWR(0x47, 0x16, struct d3dkmt_changevideomemoryreservation)
#define LX_DXCREATEHWQUEUE \
    _IOWR(0x47, 0x18, struct d3dkmt_createhwqueue)
#define LX_DXDESTROYDEVICE \
    _IOWR(0x47, 0x19, struct d3dkmt_destroydevice)
#define LX_DXDESTROYHWQUEUE \
    _IOWR(0x47, 0x1b, struct d3dkmt_destroyhwqueue)
#define LX_DXDESTROYPAGINGQUEUE \
    _IOWR(0x47, 0x1c, struct d3dddi_destroypagingqueue)
#define LX_DXDESTROYSYNCHRONIZATIONOBJECT \
    _IOWR(0x47, 0x1d, struct d3dkmt_destroysynchronizationobject)
#define LX_DXEVICT \
    _IOWR(0x47, 0x1e, struct d3dkmt_evict)
#define LX_DXFREEGPUVIRTUALADDRESS \
    _IOWR(0x47, 0x20, struct d3dkmt_freegpuvirtualaddress)
#define LX_DXFLUSHHEAPTRANSITIONS \
    _IOWR(0x47, 0x1f, struct d3dkmt_flushheaptransitions)
#define LX_DXGETCONTEXTINPROCESSSCHEDULINGPRIORITY \
    _IOWR(0x47, 0x21, struct d3dkmt_getcontextinprocessschedulingpriority)
#define LX_DXGETCONTEXTSCHEDULINGPRIORITY \
    _IOWR(0x47, 0x22, struct d3dkmt_getcontextschedulingpriority)
#define LX_DXLOCK2 \
    _IOWR(0x47, 0x25, struct d3dkmt_lock2)
#define LX_DXMARKDEVICEASERROR \
    _IOWR(0x47, 0x26, struct d3dkmt_markdeviceaserror)
#define LX_DXINVALIDATECACHE \
    _IOWR(0x47, 0x24, struct d3dkmt_invalidatecache)
#define LX_DXOFFERALLOCATIONS \
    _IOWR(0x47, 0x27, struct d3dkmt_offerallocations)
#define LX_DXRECLAIMALLOCATIONS2 \
    _IOWR(0x47, 0x2c, struct d3dkmt_reclaimallocations2)
#define LX_DXSETALLOCATIONPRIORITY \
    _IOWR(0x47, 0x2e, struct d3dkmt_setallocationpriority)
#define LX_DXSETCONTEXTINPROCESSSCHEDULINGPRIORITY \
    _IOWR(0x47, 0x2f, struct d3dkmt_setcontextinprocessschedulingpriority)
#define LX_DXSETCONTEXTSCHEDULINGPRIORITY \
    _IOWR(0x47, 0x30, struct d3dkmt_setcontextschedulingpriority)
#define LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU \
    _IOWR(0x47, 0x31, struct d3dkmt_signalsynchronizationobjectfromcpu)
#define LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU \
    _IOWR(0x47, 0x32, struct d3dkmt_signalsynchronizationobjectfromgpu)
#define LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU2 \
    _IOWR(0x47, 0x33, struct d3dkmt_signalsynchronizationobjectfromgpu2)
#define LX_DXSUBMITCOMMANDTOHWQUEUE \
    _IOWR(0x47, 0x34, struct d3dkmt_submitcommandtohwqueue)
#define LX_DXSUBMITSIGNALSYNCOBJECTSTOHWQUEUE \
    _IOWR(0x47, 0x35, struct d3dkmt_submitsignalsyncobjectstohwqueue)
#define LX_DXSUBMITWAITFORSYNCOBJECTSTOHWQUEUE \
    _IOWR(0x47, 0x36, struct d3dkmt_submitwaitforsyncobjectstohwqueue)
#define LX_DXUNLOCK2 \
    _IOWR(0x47, 0x37, struct d3dkmt_unlock2)
#define LX_DXUPDATEALLOCPROPERTY \
    _IOWR(0x47, 0x38, struct d3dddi_updateallocproperty)
#define LX_DXUPDATEGPUVIRTUALADDRESS \
    _IOWR(0x47, 0x39, struct d3dkmt_updategpuvirtualaddress)
#define LX_DXQUERYALLOCATIONRESIDENCY \
    _IOWR(0x47, 0x2a, struct d3dkmt_queryallocationresidency)
#define LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMCPU \
    _IOWR(0x47, 0x3a, struct d3dkmt_waitforsynchronizationobjectfromcpu)
#define LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMGPU \
    _IOWR(0x47, 0x3b, struct d3dkmt_waitforsynchronizationobjectfromgpu)
#define LX_DXGETALLOCATIONPRIORITY \
    _IOWR(0x47, 0x3c, struct d3dkmt_getallocationpriority)
#define LX_DXQUERYCLOCKCALIBRATION \
    _IOWR(0x47, 0x3d, struct d3dkmt_queryclockcalibration)
#define LX_DXENUMADAPTERS3 \
    _IOWR(0x47, 0x3e, struct d3dkmt_enumadapters3)
#define LX_DXSHAREOBJECTS \
    _IOWR(0x47, 0x3f, struct d3dkmt_shareobjects)
#define LX_DXOPENSYNCOBJECTFROMNTHANDLE2 \
    _IOWR(0x47, 0x40, struct d3dkmt_opensyncobjectfromnthandle2)
#define LX_DXQUERYRESOURCEINFOFROMNTHANDLE \
    _IOWR(0x47, 0x41, struct d3dkmt_queryresourceinfofromnthandle)
#define LX_DXOPENRESOURCEFROMNTHANDLE \
    _IOWR(0x47, 0x42, struct d3dkmt_openresourcefromnthandle)
#define LX_DXQUERYSTATISTICS \
    _IOWR(0x47, 0x43, struct d3dkmt_querystatistics)
#define LX_DXSHAREOBJECTWITHHOST \
    _IOWR(0x47, 0x44, struct d3dkmt_shareobjectwithhost)
#define LX_DXCREATESYNCFILE \
    _IOWR(0x47, 0x45, struct d3dkmt_createsyncfile)
#define LX_DXWAITSYNCFILE \
    _IOWR(0x47, 0x46, struct d3dkmt_waitsyncfile)
#define LX_DXOPENSYNCOBJECTFROMSYNCFILE \
    _IOWR(0x47, 0x47, struct d3dkmt_opensyncobjectfromsyncfile)
#define LX_DXENUMPROCESSES \
    _IOWR(0x47, 0x48, struct d3dkmt_enumprocesses)
#define LX_ISFEATUREENABLED \
    _IOWR(0x47, 0x49, struct d3dkmt_isfeatureenabled)

#endif
