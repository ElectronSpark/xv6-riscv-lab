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
    _KMTQAITYPE_ADAPTERTYPE = 15,
    _KMTQAITYPE_ADAPTERTYPE_RENDER = 57,
};

struct d3dkmt_queryadapterinfo {
    struct d3dkmthandle adapter;
    enum kmtqueryadapterinfotype type;
    uint64 private_data;
    uint32 private_data_size;
};

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

struct d3dkmt_destroycontext {
    struct d3dkmthandle context;
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

struct d3dkmt_freegpuvirtualaddress {
    struct d3dkmthandle adapter;
    uint32 reserved;
    uint64 base_address;
    uint64 size;
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

struct d3dkmt_destroysynchronizationobject {
    struct d3dkmthandle sync_object;
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

#define LX_DXOPENADAPTERFROMLUID \
    _IOWR(0x47, 0x01, struct d3dkmt_openadapterfromluid)
#define LX_DXCREATEDEVICE \
    _IOWR(0x47, 0x02, struct d3dkmt_createdevice)
#define LX_DXCREATECONTEXTVIRTUAL \
    _IOWR(0x47, 0x04, struct d3dkmt_createcontextvirtual)
#define LX_DXDESTROYCONTEXT \
    _IOWR(0x47, 0x05, struct d3dkmt_destroycontext)
#define LX_DXCREATEPAGINGQUEUE \
    _IOWR(0x47, 0x07, struct d3dkmt_createpagingqueue)
#define LX_DXRESERVEGPUVIRTUALADDRESS \
    _IOWR(0x47, 0x08, struct d3dddi_reservegpuvirtualaddress)
#define LX_DXQUERYADAPTERINFO \
    _IOWR(0x47, 0x09, struct d3dkmt_queryadapterinfo)
#define LX_DXQUERYVIDEOMEMORYINFO \
    _IOWR(0x47, 0x0a, struct d3dkmt_queryvideomemoryinfo)
#define LX_DXGETDEVICESTATE \
    _IOWR(0x47, 0x0e, struct d3dkmt_getdevicestate)
#define LX_DXCREATESYNCHRONIZATIONOBJECT \
    _IOWR(0x47, 0x10, struct d3dkmt_createsynchronizationobject2)
#define LX_DXENUMADAPTERS2 \
    _IOWR(0x47, 0x14, struct d3dkmt_enumadapters2)
#define LX_DXCLOSEADAPTER \
    _IOWR(0x47, 0x15, struct d3dkmt_closeadapter)
#define LX_DXDESTROYDEVICE \
    _IOWR(0x47, 0x19, struct d3dkmt_destroydevice)
#define LX_DXDESTROYPAGINGQUEUE \
    _IOWR(0x47, 0x1c, struct d3dddi_destroypagingqueue)
#define LX_DXDESTROYSYNCHRONIZATIONOBJECT \
    _IOWR(0x47, 0x1d, struct d3dkmt_destroysynchronizationobject)
#define LX_DXFREEGPUVIRTUALADDRESS \
    _IOWR(0x47, 0x20, struct d3dkmt_freegpuvirtualaddress)
#define LX_DXENUMADAPTERS3 \
    _IOWR(0x47, 0x3e, struct d3dkmt_enumadapters3)

#endif