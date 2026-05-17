#include <types.h>
#include <param.h>
#include <defs.h>
#include <errno.h>
#include <string.h>
#include <printf.h>
#include <compiler.h>
#include <dev/ps2mouse.h>
#include <dev/ps2kbd.h>
#include <dev/bio.h>
#include <dev/blkdev.h>
#include <dev/gendisk.h>
#include <dev/dev.h>
#include <dev/fdt.h>
#include <dev/net.h>
#include <dev/netdev.h>
#include <dev/cdev.h>
#include <uabi/d3dkmthk.h>
#include <vfs/file.h>
#include <vfs/fcntl.h>
#include <vfs/stat.h>
#include <lock/spinlock.h>
#include <lock/mutex_types.h>
#include <lock/mutex.h>
#include <mm/page.h>
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <arch/vm.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <cmdline.h>

#if defined(__x86_64__) || defined(__i386__)

#include "seg.h"
#include "x86.h"

extern void sleep_ms(uint64 ms);
int snprintf(char *buf, size_t size, const char *fmt, ...);
extern uint64 __timebase_frequency;
extern pagetable_t kernel_pagetable;

#define HV_MSR_GUEST_OS_ID     0x40000000
#define HV_MSR_HYPERCALL       0x40000001
#define HV_MSR_SCONTROL        0x40000080
#define HV_MSR_SIEFP           0x40000082
#define HV_MSR_SIMP            0x40000083
#define HV_MSR_EOM             0x40000084
#define HV_MSR_SINT(n)         (0x40000090 + (n))

#define HV_SYNIC_ENABLE        1ULL
#define HV_SYNIC_PAGE_ENABLE   1ULL
#define HV_SYNIC_SINT_MASKED   (1ULL << 16)
#define HV_SYNIC_SINT_AUTO_EOI (1ULL << 17)

#define HV_MESSAGE_SINT        2
#define HV_SYNIC_VECTOR        0xF1
#define HV_EVENT_FLAGS_BYTES   256
#define HVMSG_NONE             0
#define HVMSG_CHANNEL          1

#define HVCALL_POST_MESSAGE    0x005c
#define HVCALL_SIGNAL_EVENT    0x005d
#define HV_HYPERCALL_FAST      (1ULL << 16)

#define HV_STATUS_SUCCESS      0
#define HV_STATUS_INVALID_CONNECTION_ID 18
#define HV_STATUS_INSUFFICIENT_MEMORY   11
#define HV_STATUS_INSUFFICIENT_BUFFERS  19

#define VMBUS_MSG_CONN_ID      1
#define VMBUS_MSG_CONN_ID4     4
#define VMBUS_EVENT_CONN_ID    2

#define VMBUS_MAKE_VERSION(maj, min) ((((uint32)(maj)) << 16) | (min))
#define VMBUS_VERSION_WIN10_V5 VMBUS_MAKE_VERSION(5, 0)
#define VMBUS_VERSION_WIN10    VMBUS_MAKE_VERSION(4, 0)

#define CHANNELMSG_OFFERCHANNEL        1
#define CHANNELMSG_REQUESTOFFERS       3
#define CHANNELMSG_ALLOFFERS_DELIVERED 4
#define CHANNELMSG_OPENCHANNEL         5
#define CHANNELMSG_OPENCHANNEL_RESULT  6
#define CHANNELMSG_GPADL_HEADER        8
#define CHANNELMSG_GPADL_BODY          9
#define CHANNELMSG_GPADL_CREATED       10
#define CHANNELMSG_INITIATE_CONTACT    14
#define CHANNELMSG_VERSION_RESPONSE    15

#define HV_VMBUS_MSG_PAYLOAD_BYTES     240

#define VM_PKT_DATA_INBAND 0x6
#define VM_PKT_DATA_USING_XFER_PAGES 0x7
#define VM_PKT_DATA_USING_GPA_DIRECT 0x9
#define VM_PKT_COMP        0xb
#define VM_PKT_COMPLETION_REQUESTED 1

#define PIPE_MESSAGE_DATA 1

#define SYNTH_HID_PROTOCOL_REQUEST          0
#define SYNTH_HID_PROTOCOL_RESPONSE         1
#define SYNTH_HID_INITIAL_DEVICE_INFO       2
#define SYNTH_HID_INITIAL_DEVICE_INFO_ACK   3
#define SYNTH_HID_INPUT_REPORT              4
#define SYNTHHID_INPUT_VERSION              ((2U << 16) | 0U)

#define SYNTH_KBD_PROTOCOL_REQUEST          1
#define SYNTH_KBD_PROTOCOL_RESPONSE         2
#define SYNTH_KBD_EVENT                     3
#define SYNTH_KBD_VERSION                   ((1U << 16) | 0U)
#define SYNTH_KBD_STATUS_ACCEPTED           1U
#define SYNTH_KBD_IS_BREAK                  2U
#define SYNTH_KBD_IS_E0                     4U
#define SYNTH_KBD_IS_E1                     8U

#define PIPE_MSG_DATA                       1U
#define SYNTHVID_VERSION(major, minor)      (((minor) << 16) | (major))
#define SYNTHVID_VERSION_WIN8               SYNTHVID_VERSION(3, 2)
#define SYNTHVID_VERSION_WIN10              SYNTHVID_VERSION(3, 5)
#define SYNTHVID_VERSION_REQUEST            1U
#define SYNTHVID_VERSION_RESPONSE           2U
#define SYNTHVID_VRAM_LOCATION              3U
#define SYNTHVID_VRAM_LOCATION_ACK          4U
#define SYNTHVID_SITUATION_UPDATE           5U
#define SYNTHVID_FEATURE_CHANGE             9U
#define SYNTHVID_DIRT                       10U
#define SYNTHVID_RESOLUTION_REQUEST         13U
#define SYNTHVID_RESOLUTION_RESPONSE        14U
#define SYNTHVID_MAX_RESOLUTION_COUNT       64U
#define SYNTHVID_EDID_BLOCK_SIZE            128U

#define HV_SEND_PAGES 64
#define HV_RECV_PAGES 64
#define HV_RING_PAGES (HV_SEND_PAGES + HV_RECV_PAGES)
#define HV_RING_ORDER 7
#define HV_GPADL_HANDLE 0xE1E10
#define HV_KBD_GPADL_HANDLE 0xE1E11
#define HV_STOR_GPADL_HANDLE 0xE1E12
#define HV_NET_GPADL_HANDLE 0xE1E13
#define HV_VIDEO_GPADL_HANDLE 0xE1E14
#define HV_DXG_GLOBAL_GPADL_HANDLE 0xE1E15
#define HV_DXG_VGPU_GPADL_HANDLE 0xE1E16
#define HV_NET_RECV_GPADL_HANDLE 0x21
#define HV_NET_SEND_GPADL_HANDLE 0x22
#define HV_WAIT_LOOPS 500
#define HV_STOR_WAIT_LOOPS 2000
#define HV_STOR_INIT_TIMEOUT_MS 5000
#define HV_STOR_IO_TIMEOUT_MS   65000
#define HV_VIDEO_DIRTY_INTERVAL_MS 16
#define HV_VIDEO_REFRESH_INTERVAL_MS 1000
#define HV_NET_WAIT_LOOPS 2000
#define HV_STOR_MAX_PAGES 32

#define HV_DXG_VM_BUS_PACKET_MAX (128U * 1024U)
#define HV_DXG_WAIT_MS 5000
#define HV_DXG_RESULT_BYTES HV_DXG_VM_BUS_PACKET_MAX
#define HV_DXG_PACKET_BYTES (HV_DXG_VM_BUS_PACKET_MAX + 256U)
#define HV_DXG_PREFIX_BYTES 64
#define HV_DXG_VMBUS_INTERFACE_VERSION_OLD 27U
#define HV_DXG_VMBUS_INTERFACE_VERSION 40U
#define HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION 16U
#define HV_DXGK_VMBCOMMAND_CREATEDEVICE 0U
#define HV_DXGK_VMBCOMMAND_DESTROYDEVICE 1U
#define HV_DXGK_VMBCOMMAND_CREATEALLOCATION 4U
#define HV_DXGK_VMBCOMMAND_DESTROYALLOCATION 5U
#define HV_DXGK_VMBCOMMAND_OPENSYNCOBJECT 1002U
#define HV_DXGK_VMBCOMMAND_CREATEPROCESS 1000U
#define HV_DXGK_VMBCOMMAND_DESTROYPROCESS 1001U
#define HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT 1003U
#define HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT 1004U
#define HV_DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT 1005U
#define HV_DXGK_VMBCOMMAND_SETIOSPACEREGION 1010U
#define HV_DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST 1021U
#define HV_DXGK_VMBCOMMAND_ISFEATUREENABLED_GLOBAL 1022U
#define HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO 2U
#define HV_DXGK_VMBCOMMAND_CREATESYNCOBJECT 8U
#define HV_DXGK_VMBCOMMAND_CREATEPAGINGQUEUE 9U
#define HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE 10U
#define HV_DXGK_VMBCOMMAND_MAKERESIDENT 11U
#define HV_DXGK_VMBCOMMAND_EVICT 12U
#define HV_DXGK_VMBCOMMAND_ESCAPE 13U
#define HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS 16U
#define HV_DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS 17U
#define HV_DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS 18U
#define HV_DXGK_VMBCOMMAND_UPDATEGPUVIRTUALADDRESS 19U
#define HV_DXGK_VMBCOMMAND_SUBMITCOMMAND 20U
#define HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMCPU 22U
#define HV_DXGK_VMBCOMMAND_LOCK2 23U
#define HV_DXGK_VMBCOMMAND_UNLOCK2 24U
#define HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU 25U
#define HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT 26U
#define HV_DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL 6U
#define HV_DXGK_VMBCOMMAND_DESTROYCONTEXT 7U
#define HV_DXGK_VMBCOMMAND_GETDEVICESTATE 28U
#define HV_DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA 39U
#define HV_DXGK_VMBCOMMAND_OPENRESOURCE 32U
#define HV_DXGK_VMBCOMMAND_SETCONTEXTSCHEDULINGPRIORITY 33U
#define HV_DXGK_VMBCOMMAND_FLUSHHEAPTRANSITIONS 37U
#define HV_DXGK_VMBCOMMAND_QUERYALLOCATIONRESIDENCY 41U
#define HV_DXGK_VMBCOMMAND_SETEXISTINGSYSMEMSTORE 45U
#define HV_DXGK_VMBCOMMAND_QUERYSTATISTICS 48U
#define HV_DXGK_VMBCOMMAND_CHANGEVIDEOMEMORYRESERVATION 49U
#define HV_DXGK_VMBCOMMAND_OFFERALLOCATIONS 57U
#define HV_DXGK_VMBCOMMAND_RECLAIMALLOCATIONS 58U
#define HV_DXGK_VMBCOMMAND_OPENADAPTER 14U
#define HV_DXGK_VMBCOMMAND_CLOSEADAPTER 15U
#define HV_DXGK_VMBCOMMAND_QUERYVIDEOMEMORYINFO 21U
#define HV_DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO 36U
#define HV_DXGK_VMBCOMMAND_CREATEHWQUEUE 50U
#define HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE 51U
#define HV_DXGK_VMBCOMMAND_SUBMITCOMMANDTOHWQUEUE 52U
#define HV_DXGK_VMBCOMMAND_UPDATEALLOCATIONPROPERTY 56U
#define HV_DXGK_VMBCOMMAND_SETALLOCATIONPRIORITY 59U
#define HV_DXGK_VMBCOMMAND_GETALLOCATIONPRIORITY 60U
#define HV_DXGK_VMBCOMMAND_GETCONTEXTSCHEDULINGPRIORITY 61U
#define HV_DXGK_VMBCOMMAND_QUERYCLOCKCALIBRATION 62U
#define HV_DXGK_VMBCOMMAND_QUERYRESOURCEINFO 64U
#define HV_DXGK_VMBCOMMAND_SETEXISTINGSYSMEMPAGES 66U
#define HV_DXGK_VMBCOMMAND_INVALIDATECACHE 67U
#define HV_DXGK_VMBCOMMAND_ISFEATUREENABLED 68U
#define HV_DXGK_VMBCOMMAND_SIGNALGUESTEVENT 0U
#define HV_DXGK_VMBCOMMAND_SETGUESTDATA 2U
#define HV_DXGK_VMBCOMMAND_SIGNALGUESTEVENTPASSIVE 3U
#define HV_DXGK_VMBCOMMAND_SENDWNFNOTIFICATION 4U
#define HV_DXGKVMB_VM_TO_HOST 1U
#define HV_DXGKVMB_VGPU_TO_HOST 0U
#define HV_DXG_PROCESS_NAME_LENGTH 260U
#define HV_DXG_IOCTL_PRIVATE_MAX 1024U
#define HV_DXG_ALLOCATION_MAX 8U
#define HV_DXG_RESOURCE_TRACKED_MAX 64U
#define HV_DXG_RESIDENCY_ALLOCATION_MAX (1024U * 10U)
#define HV_DXG_SYSMEM_PFNS_PER_PACKET 128U
#define HV_DXG_GPUVA_UPDATE_MAX 16U
#define HV_DXG_HISTORY_BUFFER_MAX 64U
#define HV_DXG_OPEN_TRACKED_MAX 512U
#define HV_DXG_HOST_EVENT_MAX 64U
#define HV_DXG_HOST_EVENT_TIMEOUT_MS 65000U
#define HV_DXG_STATUS_BUF_SIZE (32U * 1024U)
#define HV_DXG_QUERY_HISTORY_MAX 16U
#define HV_DXG_IOCTL_HISTORY_MAX 16U
#define HV_DXG_IOCTL_NR_MAX 256U
#define HV_DXG_IOCTL_TIME_TOP 4U
#define HV_DXG_RESOURCE_HISTORY_MAX 8U
#define HV_DXG_ALLOCATION_FLAG_CACHED (1U << 19)
#define HV_DXG_SYNC_SPIN_POLLS 4096U
#define HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID 31U
#define HV_DXG_VENDOR_INTEL 0x8086U
#define HV_DXG_VENDOR_NVIDIA 0x10deU
#define HV_DXG_IOSPACE_ALIGN 0x10000ULL
#define HV_DXG_IOSPACE_SEARCH_START 0x100000000ULL
#define HV_DXG_IOSPACE_SEARCH_END   0x8000000000ULL

#define NVSP_PROTOCOL_VERSION_1  2U
#define NVSP_PROTOCOL_VERSION_2  0x30002U
#define NVSP_PROTOCOL_VERSION_4  0x40000U
#define NVSP_PROTOCOL_VERSION_5  0x50000U
#define NVSP_PROTOCOL_VERSION_6  0x60000U
#define NVSP_PROTOCOL_VERSION_61 0x60001U

#define NVSP_MSG_TYPE_INIT                     1U
#define NVSP_MSG_TYPE_INIT_COMPLETE            2U
#define NVSP_VERSION_MSG_START                 100U
#define NVSP_MSG1_TYPE_SEND_NDIS_VER           (NVSP_VERSION_MSG_START)
#define NVSP_MSG1_TYPE_SEND_RECV_BUF           (NVSP_VERSION_MSG_START + 1U)
#define NVSP_MSG1_TYPE_SEND_RECV_BUF_COMPLETE  (NVSP_VERSION_MSG_START + 2U)
#define NVSP_MSG1_TYPE_SEND_SEND_BUF           (NVSP_VERSION_MSG_START + 4U)
#define NVSP_MSG1_TYPE_SEND_SEND_BUF_COMPLETE  (NVSP_VERSION_MSG_START + 5U)
#define NVSP_MSG1_TYPE_SEND_RNDIS_PKT          (NVSP_VERSION_MSG_START + 7U)
#define NVSP_MSG1_TYPE_SEND_RNDIS_PKT_COMPLETE (NVSP_VERSION_MSG_START + 8U)
#define NVSP_MSG2_TYPE_SEND_NDIS_CONFIG        (NVSP_VERSION_MSG_START + 25U)
#define NVSP_STAT_SUCCESS                      1U
#define NVSP_STAT_FAIL                         2U
#define NVSP_MESSAGE_WIRE_SIZE                 40U
#define NETVSC_RECEIVE_BUFFER_ID               0xcafeU
#define NETVSC_SEND_BUFFER_ID                  0U
#define NETVSC_INVALID_INDEX                   0xffffffffU
#define NETVSC_RECV_SECTION_SIZE               1728U
#define NETVSC_SEND_SECTION_SIZE               6144U
#define HV_NET_RECV_ORDER                      11U
#define HV_NET_RECV_SIZE                       ((1U << HV_NET_RECV_ORDER) * PGSIZE)
#define HV_NET_SEND_ORDER                      8U
#define HV_NET_SEND_SIZE                       ((1U << HV_NET_SEND_ORDER) * PGSIZE)
#define HV_NET_MAX_SEND_SECTIONS               256U
#define HV_NET_TX_SECTION_TRANS_BASE           0x6e74787300000000ULL
#define HV_NET_TX_SECTION_TRANS_MASK           0xffffffff00000000ULL

#define RNDIS_MAJOR_VERSION                    1U
#define RNDIS_MINOR_VERSION                    0U
#define RNDIS_MSG_PACKET                       0x00000001U
#define RNDIS_MSG_INIT                         0x00000002U
#define RNDIS_MSG_INIT_C                       0x80000002U
#define RNDIS_MSG_QUERY                        0x00000004U
#define RNDIS_MSG_QUERY_C                      0x80000004U
#define RNDIS_MSG_SET                          0x00000005U
#define RNDIS_MSG_SET_C                        0x80000005U
#define RNDIS_MSG_INDICATE                     0x00000007U
#define RNDIS_STATUS_SUCCESS                   0x00000000U
#define RNDIS_STATUS_MEDIA_CONNECT             0x4001000bU
#define RNDIS_STATUS_MEDIA_DISCONNECT          0x4001000cU
#define RNDIS_OID_GEN_LINK_SPEED               0x00010107U
#define RNDIS_OID_GEN_CURRENT_PACKET_FILTER    0x0001010eU
#define RNDIS_OID_GEN_MEDIA_CONNECT_STATUS     0x00010114U
#define RNDIS_OID_802_3_PERMANENT_ADDRESS      0x01010101U
#define RNDIS_OID_802_3_CURRENT_ADDRESS        0x01010102U
#define RNDIS_PACKET_TYPE_DIRECTED             0x00000001U
#define RNDIS_PACKET_TYPE_MULTICAST            0x00000002U
#define RNDIS_PACKET_TYPE_BROADCAST            0x00000008U
#define RNDIS_MEDIA_STATE_CONNECTED            0x00000000U

#define VSTOR_OPERATION_COMPLETE_IO          1
#define VSTOR_OPERATION_EXECUTE_SRB          3
#define VSTOR_OPERATION_BEGIN_INITIALIZATION 7
#define VSTOR_OPERATION_END_INITIALIZATION   8
#define VSTOR_OPERATION_QUERY_PROTOCOL       9
#define VSTOR_OPERATION_QUERY_PROPERTIES     10
#define VMSTOR_PROTOCOL_VERSION(maj, min) ((((maj) & 0xff) << 8) | ((min) & 0xff))
#define VMSTOR_PROTOCOL_WIN10  VMSTOR_PROTOCOL_VERSION(6, 2)
#define VMSTOR_PROTOCOL_WIN8_1 VMSTOR_PROTOCOL_VERSION(6, 0)
#define VMSTOR_PROTOCOL_WIN8   VMSTOR_PROTOCOL_VERSION(5, 1)
#define VMSTOR_PROTOCOL_WIN7   VMSTOR_PROTOCOL_VERSION(4, 2)
#define REQUEST_COMPLETION_FLAG 0x1
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000008
#define SRB_FLAGS_DATA_IN                0x00000040
#define SRB_FLAGS_DATA_OUT               0x00000080
#define SRB_FLAGS_NO_DATA_TRANSFER       0x00000000

#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2a
#define SCSI_SYNCHRONIZE_CACHE  0x35
#define SRB_STATUS_AUTOSENSE_VALID 0x80
#define SRB_STATUS_QUEUE_FROZEN    0x40
#define SRB_STATUS_SUCCESS      0x01
#define SRB_STATUS(status) ((status) & ~(SRB_STATUS_AUTOSENSE_VALID | SRB_STATUS_QUEUE_FROZEN))
#define STORVSC_DATA_WRITE      0
#define STORVSC_DATA_READ       1
#define STORVSC_DATA_NONE       2

struct hv_guid {
    uint32 a;
    uint16 b;
    uint16 c;
    uint8 d[8];
} __PACKED;

static const struct hv_guid hv_mouse_guid = {
    0xcfa8b69e, 0x5b4a, 0x4cc0,
    { 0xb9, 0x8b, 0x8b, 0xa1, 0xa1, 0xf3, 0xf9, 0x5a }
};

static const struct hv_guid hv_kbd_guid = {
    0xf912ad6d, 0x2b17, 0x48ea,
    { 0xbd, 0x65, 0xf9, 0x27, 0xa6, 0x1c, 0x76, 0x84 }
};

static const struct hv_guid hv_scsi_guid = {
    0xba6163d9, 0x04a1, 0x4d29,
    { 0xb6, 0x05, 0x72, 0xe2, 0xff, 0xb1, 0xdc, 0x7f }
};

static const struct hv_guid hv_ide_guid = {
    0x32412632, 0x86cb, 0x44a2,
    { 0x9b, 0x5c, 0x50, 0xd1, 0x41, 0x73, 0x54, 0xf5 }
};

static const struct hv_guid hv_net_guid = {
    0xf8615163, 0xdf3e, 0x46c5,
    { 0x91, 0x3f, 0xf2, 0xd2, 0xf9, 0x65, 0xed, 0x0e }
};

static const struct hv_guid hv_video_guid = {
    0xda0a7802, 0xe377, 0x4aac,
    { 0x8e, 0x77, 0x05, 0x58, 0xeb, 0x10, 0x73, 0xf8 }
};

static const struct hv_guid hv_dxg_global_guid = {
    0xdde9cbc0, 0x5060, 0x4436,
    { 0x94, 0x48, 0xea, 0x12, 0x54, 0xa5, 0xd1, 0x77 }
};

static const struct hv_guid hv_dxg_vgpu_guid = {
    0x6e382d18, 0x3336, 0x4f4b,
    { 0xac, 0xc4, 0x2b, 0x77, 0x03, 0xd4, 0xdf, 0x4a }
};

struct hv_message_header {
    uint32 message_type;
    uint8 payload_size;
    uint8 message_flags;
    uint8 reserved[2];
    uint64 sender;
} __PACKED;

struct hv_message {
    struct hv_message_header header;
    uint64 payload[30];
} __PACKED;

struct hv_input_post_message {
    uint32 connection_id;
    uint32 reserved;
    uint32 message_type;
    uint32 payload_size;
    uint64 payload[30];
} __PACKED;

struct vmbus_msg_hdr {
    uint32 msgtype;
    uint32 padding;
} __PACKED;

struct vmbus_initiate_contact {
    struct vmbus_msg_hdr header;
    uint32 version;
    uint32 target_vcpu;
    union {
        uint64 interrupt_page;
        struct {
            uint8 msg_sint;
            uint8 msg_vtl;
            uint8 reserved[2];
            uint32 feature_flags;
        };
    };
    uint64 monitor_page1;
    uint64 monitor_page2;
} __PACKED;

struct vmbus_version_response {
    struct vmbus_msg_hdr header;
    uint8 supported;
    uint8 connection_state;
    uint16 padding;
    uint32 msg_conn_id;
} __PACKED;

struct vmbus_offer {
    struct hv_guid if_type;
    struct hv_guid if_instance;
    uint64 reserved1;
    uint64 reserved2;
    uint16 flags;
    uint16 mmio_megabytes;
    uint8 user_def[120];
    uint16 sub_channel_index;
    uint16 reserved3;
} __PACKED;

struct vmbus_offer_channel {
    struct vmbus_msg_hdr header;
    struct vmbus_offer offer;
    uint32 child_relid;
    uint8 monitorid;
    uint8 monitor_allocated;
    uint16 dedicated;
    uint32 connection_id;
} __PACKED;

struct vmbus_gpa_range {
    uint32 byte_count;
    uint32 byte_offset;
    uint64 pfn[HV_RING_PAGES];
} __PACKED;

struct vmbus_gpadl_header {
    struct vmbus_msg_hdr header;
    uint32 child_relid;
    uint32 gpadl;
    uint16 range_buflen;
    uint16 rangecount;
    struct vmbus_gpa_range range;
} __PACKED;

struct vmbus_gpadl_created {
    struct vmbus_msg_hdr header;
    uint32 child_relid;
    uint32 gpadl;
    uint32 status;
} __PACKED;

#define HV_GPADL_HEADER_MAX_PFNS \
    ((HV_VMBUS_MSG_PAYLOAD_BYTES - sizeof(struct vmbus_msg_hdr) - \
      sizeof(uint32) - sizeof(uint32) - sizeof(uint16) - sizeof(uint16) - \
      sizeof(uint32) - sizeof(uint32)) / sizeof(uint64))
#define HV_GPADL_BODY_MAX_PFNS \
    ((HV_VMBUS_MSG_PAYLOAD_BYTES - sizeof(struct vmbus_msg_hdr) - \
      sizeof(uint32) - sizeof(uint32)) / sizeof(uint64))

struct vmbus_gpadl_header_large {
    struct vmbus_msg_hdr header;
    uint32 child_relid;
    uint32 gpadl;
    uint16 range_buflen;
    uint16 rangecount;
    uint32 byte_count;
    uint32 byte_offset;
    uint64 pfn[HV_GPADL_HEADER_MAX_PFNS];
} __PACKED;

struct vmbus_gpadl_body {
    struct vmbus_msg_hdr header;
    uint32 msgnumber;
    uint32 gpadl;
    uint64 pfn[HV_GPADL_BODY_MAX_PFNS];
} __PACKED;

struct vmbus_open_channel {
    struct vmbus_msg_hdr header;
    uint32 child_relid;
    uint32 openid;
    uint32 ringbuffer_gpadlhandle;
    uint32 target_vp;
    uint32 downstream_ringbuffer_pageoffset;
    uint8 userdata[120];
} __PACKED;

struct vmbus_open_result {
    struct vmbus_msg_hdr header;
    uint32 child_relid;
    uint32 openid;
    uint32 status;
} __PACKED;

struct hv_ring_buffer {
    uint32 write_index;
    uint32 read_index;
    uint32 interrupt_mask;
    uint32 pending_send_sz;
    uint32 reserved1[12];
    uint32 feature_bits;
    uint8 reserved2[4096 - 68];
    uint8 buffer[];
} __PACKED;

struct hv_monitor_trigger_group {
    uint32 pending;
    uint32 armed;
} __PACKED;

struct hv_monitor_page {
    uint32 trigger_state;
    uint32 reserved1;
    struct hv_monitor_trigger_group trigger_group[4];
    uint64 reserved2[3];
    int32 next_checktime[4][32];
    uint16 latency[4][32];
    uint64 reserved3[32];
    uint8 reserved4[1984];
} __PACKED;

struct vmpacket_descriptor {
    uint16 type;
    uint16 offset8;
    uint16 len8;
    uint16 flags;
    uint64 trans_id;
} __PACKED;

struct vmtransfer_page_range {
    uint32 byte_count;
    uint32 byte_offset;
} __PACKED;

struct vmtransfer_page_packet_header {
    struct vmpacket_descriptor d;
    uint16 xfer_pageset_id;
    uint8 sender_owns_set;
    uint8 reserved;
    uint32 range_cnt;
    struct vmtransfer_page_range ranges[];
} __PACKED;

struct hv_multipage_buffer {
    uint32 len;
    uint32 offset;
    uint64 pfn_array[HV_STOR_MAX_PAGES];
} __PACKED;

struct vmbus_packet_multipage_buffer {
    uint16 type;
    uint16 offset8;
    uint16 len8;
    uint16 flags;
    uint64 trans_id;
    uint32 reserved;
    uint32 range_count;
    struct hv_multipage_buffer range;
} __PACKED;

struct hvdxg_winluid {
    uint32 a;
    uint32 b;
} __PACKED;

struct hvdxg_ext_header {
    uint32 command_offset;
    uint32 reserved;
    struct hvdxg_winluid vgpu_luid;
} __PACKED;

struct hvdxg_d3dkmthandle {
    uint32 v;
} __PACKED;

struct hvdxg_ntstatus {
    int32 v;
} __PACKED;

struct hvdxg_command_vgpu_to_host {
    uint64 command_id;
    struct hvdxg_d3dkmthandle process;
    uint32 channel_type : 8;
    uint32 async_msg : 1;
    uint32 reserved : 23;
    uint32 command_type;
    uint32 alignment_padding;
} __PACKED;

struct hvdxg_command_vm_to_host {
    uint64 command_id;
    struct hvdxg_d3dkmthandle process;
    uint32 channel_type;
    uint32 command_type;
} __PACKED;

struct hvdxg_command_host_to_vm {
    uint64 command_id;
    struct hvdxg_d3dkmthandle process;
    uint32 channel_type;
    uint32 command_type;
    uint32 alignment_padding;
} __PACKED;

struct hvdxg_command_signalguestevent {
    struct hvdxg_command_host_to_vm hdr;
    uint64 event;
    uint64 process_id;
    uint8 dereference_event;
    uint8 reserved0[3];
    struct hvdxg_d3dkmthandle context;
    struct hvdxg_d3dkmthandle fence_object;
    uint32 num_operations;
    uint32 flags;
} __PACKED;

struct hvdxg_command_createprocess {
    struct hvdxg_command_vm_to_host hdr;
    uint64 process;
    uint64 process_id;
    uint16 process_name[HV_DXG_PROCESS_NAME_LENGTH + 1];
    uint8 flags;
    uint8 reserved[5];
} __PACKED;

struct hvdxg_command_createprocess_return {
    struct hvdxg_d3dkmthandle hprocess;
} __PACKED;

struct hvdxg_command_destroyprocess {
    struct hvdxg_command_vm_to_host hdr;
} __PACKED;

struct hvdxg_command_setiospaceregion {
    struct hvdxg_command_vm_to_host hdr;
    uint32 alignment_padding;
    uint64 start;
    uint64 length;
    uint32 shared_page_gpadl;
    uint32 reserved;
} __PACKED;

struct hvdxg_command_setexistingsysmemstore {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle allocation;
    uint32 gpadl;
} __PACKED;

struct hvdxg_command_setexistingsysmempages {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle allocation;
    uint32 num_pages;
    uint32 alloc_offset_in_pages;
    uint64 pfn[1];
} __PACKED;

struct hvdxg_command_openadapter {
    struct hvdxg_command_vgpu_to_host hdr;
    uint32 vmbus_interface_version;
    uint32 vmbus_last_compatible_interface_version;
    struct hvdxg_winluid guest_adapter_luid;
} __PACKED;

struct hvdxg_command_openadapter_return {
    struct hvdxg_d3dkmthandle host_adapter_handle;
    struct hvdxg_ntstatus status;
    uint32 vmbus_interface_version;
    uint32 vmbus_last_compatible_interface_version;
} __PACKED;

struct hvdxg_command_getinternaladapterinfo {
    struct hvdxg_command_vgpu_to_host hdr;
} __PACKED;

struct hvdxg_command_closeadapter {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle host_handle;
} __PACKED;

struct hvdxg_command_queryadapterinfo {
    struct hvdxg_command_vgpu_to_host hdr;
    uint32 query_type;
    uint32 private_data_size;
    uint8 private_data[1];
} __PACKED;

struct hvdxg_command_setallocationpriority {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle resource;
    uint32 allocation_count;
    uint8 data[1];
} __PACKED;

struct hvdxg_command_getallocationpriority {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle resource;
    uint32 allocation_count;
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_getallocationpriority_return {
    struct hvdxg_ntstatus status;
    uint32 priorities[1];
} __PACKED;

struct hvdxg_command_queryallocationresidency {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_queryallocationresidency args;
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_queryallocationresidency_return {
    struct hvdxg_ntstatus status;
    enum d3dkmt_allocationresidencystatus residency_status[1];
} __PACKED;

struct hvdxg_command_escape {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle adapter;
    struct hvdxg_d3dkmthandle device;
    enum d3dkmt_escapetype type;
    struct d3dddi_escapeflags flags;
    uint32 priv_drv_data_size;
    struct hvdxg_d3dkmthandle context;
    uint8 priv_drv_data[1];
} __PACKED;

struct hvdxg_command_shareobjectwithhost {
    struct hvdxg_command_vm_to_host hdr;
    uint32 alignment_padding;
    struct hvdxg_d3dkmthandle device_handle;
    struct hvdxg_d3dkmthandle object_handle;
    uint64 reserved;
} __PACKED;

struct hvdxg_command_shareobjectwithhost_return {
    struct hvdxg_ntstatus status;
    uint32 alignment;
    uint64 vail_nt_handle;
} __PACKED;

struct hvdxg_command_createntsharedobject {
    struct hvdxg_command_vm_to_host hdr;
    uint32 alignment_padding;
    struct hvdxg_d3dkmthandle object;
} __PACKED;

struct hvdxg_command_destroyntsharedobject {
    struct hvdxg_command_vm_to_host hdr;
    uint32 alignment_padding;
    struct hvdxg_d3dkmthandle shared_handle;
} __PACKED;

struct hvdxg_command_querystatistics {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_querystatistics args;
} __PACKED;

struct hvdxg_command_querystatistics_return {
    struct hvdxg_ntstatus status;
    uint32 reserved;
    struct d3dkmt_querystatistics_result result;
} __PACKED;

struct hvdxg_command_queryclockcalibration {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_queryclockcalibration args;
} __PACKED;

struct hvdxg_command_queryclockcalibration_return {
    struct hvdxg_ntstatus status;
    struct dxgk_gpuclockdata clock_data;
} __PACKED;

struct hvdxg_command_updateallocationproperty {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dddi_updateallocproperty args;
} __PACKED;

struct hvdxg_command_updateallocationproperty_return {
    uint64 paging_fence_value;
    struct hvdxg_ntstatus status;
} __PACKED;

struct hvdxg_command_offerallocations {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    uint32 allocation_count;
    enum d3dkmt_offer_priority priority;
    struct d3dkmt_offer_flags flags;
    uint8 resources;
    uint8 reserved[3];
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_reclaimallocations {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle paging_queue;
    uint32 allocation_count;
    uint8 resources;
    uint8 write_results;
    uint8 reserved[2];
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_reclaimallocations_return {
    uint64 paging_fence_value;
    struct hvdxg_ntstatus status;
    enum d3dddi_reclaim_result results[1];
} __PACKED;

struct hvdxg_command_flushheaptransitions {
    struct hvdxg_command_vgpu_to_host hdr;
} __PACKED;

struct hvdxg_command_invalidatecache {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle allocation;
    uint64 offset;
    uint64 length;
    uint64 reserved;
} __PACKED;

struct hvdxg_command_setcontextschedulingpriority {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle context;
    int32 priority;
    uint8 in_process;
    uint8 reserved[3];
} __PACKED;

struct hvdxg_command_getcontextschedulingpriority {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle context;
    uint8 in_process;
    uint8 reserved[3];
} __PACKED;

struct hvdxg_command_getcontextschedulingpriority_return {
    struct hvdxg_ntstatus status;
    int32 priority;
} __PACKED;

struct hvdxg_feature_desc {
    uint16 min_supported_version;
    uint16 max_supported_version;
    union {
        struct {
            uint16 supported : 1;
            uint16 virtualization_mode : 3;
            uint16 global : 1;
            uint16 driver_feature : 1;
            uint16 internal : 1;
            uint16 reserved : 9;
        };
        uint16 value;
    };
} __PACKED;

struct hvdxg_command_isfeatureenabled {
    struct hvdxg_command_vgpu_to_host hdr;
    enum dxgk_feature_id feature_id;
} __PACKED;

struct hvdxg_command_isfeatureenabled_global {
    struct hvdxg_command_vm_to_host hdr;
    enum dxgk_feature_id feature_id;
} __PACKED;

struct hvdxg_command_isfeatureenabled_return {
    struct hvdxg_ntstatus status;
    struct hvdxg_feature_desc descriptor;
    struct dxgk_isfeatureenabled_result result;
} __PACKED;

struct hvdxg_command_createdevice {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_createdeviceflags flags;
    uint8 cdd_device;
    uint8 reserved[3];
    uint64 error_code;
} __PACKED;

struct hvdxg_command_createdevice_return {
    struct hvdxg_d3dkmthandle device;
} __PACKED;

struct hvdxg_command_destroydevice {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
} __PACKED;

struct hvdxg_command_createallocation_allocinfo {
    uint32 flags;
    uint32 priv_drv_data_size;
    uint32 vidpn_source_id;
} __PACKED;

struct hvdxg_rational {
    uint32 numerator;
    uint32 denominator;
} __PACKED;

struct hvdxg_describeallocation {
    uint64 allocation;
    uint32 width;
    uint32 height;
    uint32 format;
    uint32 multisample_method;
    struct hvdxg_rational refresh_rate;
    uint32 private_driver_attribute;
    uint32 flags;
    uint32 rotation;
    uint32 reserved;
} __PACKED;

struct hvdxg_command_allocinfo_return {
    struct hvdxg_d3dkmthandle allocation;
    uint32 priv_drv_data_size;
    uint32 allocation_flags;
    uint32 reserved;
    uint64 allocation_size;
    struct hvdxg_describeallocation driver_info;
} __PACKED;

struct hvdxg_command_createallocation {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle resource;
    uint32 private_runtime_data_size;
    uint32 priv_drv_data_size;
    uint32 alloc_count;
    struct d3dkmt_createallocationflags flags;
    uint64 private_runtime_resource_handle;
    uint8 make_resident;
    uint8 reserved[7];
} __PACKED;

struct hvdxg_command_createallocation_return {
    struct d3dkmt_createallocationflags flags;
    struct hvdxg_d3dkmthandle resource;
    struct hvdxg_d3dkmthandle global_share;
    uint32 vgpu_flags;
    struct hvdxg_command_allocinfo_return allocation_info[1];
} __PACKED;

struct hvdxg_command_openresource {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    uint8 nt_security_sharing;
    uint8 reserved[3];
    struct hvdxg_d3dkmthandle global_share;
    uint32 allocation_count;
    uint32 total_priv_drv_data_size;
} __PACKED;

struct hvdxg_command_openresource_return {
    struct hvdxg_d3dkmthandle resource;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_destroyallocation {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle resource;
    uint32 alloc_count;
    struct d3dddicb_destroyallocation2flags flags;
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_getdevicestate {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_getdevicestate args;
} __PACKED;

struct hvdxg_command_getdevicestate_return {
    struct d3dkmt_getdevicestate args;
    struct hvdxg_ntstatus status;
} __PACKED;

struct hvdxg_command_getstandardallocprivdata {
    struct hvdxg_command_vgpu_to_host hdr;
    enum d3dkmdt_standardallocationtype alloc_type;
    uint32 priv_driver_data_size;
    uint32 priv_driver_resource_size;
    uint32 physical_adapter_index;
    union {
        struct d3dkmdt_gdisurfacedata gdi_surface;
    };
} __PACKED;

struct hvdxg_command_getstandardallocprivdata_return {
    struct hvdxg_ntstatus status;
    uint32 priv_driver_data_size;
    uint32 priv_driver_resource_size;
    union {
        struct d3dkmdt_gdisurfacedata gdi_surface;
    };
} __PACKED;

struct hvdxg_command_createcontextvirtual {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle context;
    struct hvdxg_d3dkmthandle device;
    uint32 node_ordinal;
    uint32 engine_affinity;
    struct d3dddi_createcontextflags flags;
    uint32 client_hint;
    uint32 priv_drv_data_size;
    uint8 priv_drv_data[1];
    uint8 priv_drv_data_tail_padding[3];
} __PACKED;

struct hvdxg_command_destroycontext {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle context;
} __PACKED;

struct hvdxg_command_createhwqueue {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_ntstatus status;
    struct hvdxg_d3dkmthandle hwqueue;
    struct hvdxg_d3dkmthandle hwqueue_progress_fence;
    uint32 reserved0;
    uint64 hwqueue_progress_fence_cpuva;
    uint64 hwqueue_progress_fence_gpuva;
    struct hvdxg_d3dkmthandle context;
    struct d3dddi_createhwqueueflags flags;
    uint32 priv_drv_data_size;
    uint8 priv_drv_data[1];
    uint8 priv_drv_data_tail_padding[3];
} __PACKED;

struct hvdxg_command_destroyhwqueue {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle hwqueue;
} __PACKED;

struct hvdxg_command_createpagingqueue {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_createpagingqueue args;
} __PACKED;

struct hvdxg_command_createpagingqueue_return {
    struct hvdxg_d3dkmthandle paging_queue;
    struct hvdxg_d3dkmthandle sync_object;
    uint64 fence_storage_physical_address;
    uint64 fence_storage_offset;
} __PACKED;

struct hvdxg_command_destroypagingqueue {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle paging_queue;
} __PACKED;

struct hvdxg_command_makeresident {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle paging_queue;
    struct d3dddi_makeresident_flags flags;
    uint32 alloc_count;
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_makeresident_return {
    uint64 paging_fence_value;
    uint64 num_bytes_to_trim;
    struct hvdxg_ntstatus status;
} __PACKED;

struct hvdxg_command_evict {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    struct d3dddi_evict_flags flags;
    uint32 alloc_count;
    struct hvdxg_d3dkmthandle allocations[1];
} __PACKED;

struct hvdxg_command_evict_return {
    uint64 num_bytes_to_trim;
} __PACKED;

struct hvdxg_command_submitcommand {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_submitcommand args;
} __PACKED;

struct hvdxg_command_submitcommandtohwqueue {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_submitcommandtohwqueue args;
} __PACKED;

struct hvdxg_command_lock2 {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_lock2 args;
    uint8 use_legacy_lock;
    uint8 reserved0[3];
    uint32 flags;
    uint32 priv_drv_data;
} __PACKED;

struct hvdxg_command_lock2_return {
    struct hvdxg_ntstatus status;
    uint32 reserved;
    uint64 cpu_visible_buffer_offset;
} __PACKED;

struct hvdxg_command_unlock2 {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_unlock2 args;
    uint8 use_legacy_unlock;
    uint8 reserved0[3];
} __PACKED;

struct hvdxg_command_reservegpuvirtualaddress {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dddi_reservegpuvirtualaddress args;
} __PACKED;

struct hvdxg_command_reservegpuvirtualaddress_return {
    uint64 virtual_address;
    uint64 paging_fence_value;
} __PACKED;

struct hvdxg_command_freegpuvirtualaddress {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_freegpuvirtualaddress args;
} __PACKED;

struct hvdxg_command_mapgpuvirtualaddress {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dddi_mapgpuvirtualaddress args;
    struct hvdxg_d3dkmthandle device;
} __PACKED;

struct hvdxg_command_mapgpuvirtualaddress_return {
    uint64 virtual_address;
    uint64 paging_fence_value;
    struct hvdxg_ntstatus status;
} __PACKED;

struct hvdxg_command_updategpuvirtualaddress {
    struct hvdxg_command_vgpu_to_host hdr;
    uint64 fence_value;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle context;
    struct hvdxg_d3dkmthandle fence_object;
    uint32 num_operations;
    uint32 flags;
    struct d3dddi_updategpuvirtualaddress_operation operations[1];
} __PACKED;

struct hvdxg_command_createsyncobject {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_createsynchronizationobject2 args;
    uint32 client_hint;
} __PACKED;

struct hvdxg_command_createsyncobject_return {
    struct hvdxg_d3dkmthandle sync_object;
    struct hvdxg_d3dkmthandle global_sync_object;
    uint64 fence_gpu_va;
    uint64 fence_storage_address;
    uint32 fence_storage_offset;
} __PACKED;

struct hvdxg_command_opensyncobject {
    struct hvdxg_command_vm_to_host hdr;
    uint32 alignment_padding;
    struct hvdxg_d3dkmthandle device;
    struct hvdxg_d3dkmthandle global_sync_object;
    uint32 engine_affinity;
    struct d3dddi_synchronizationobject_flags flags;
} __PACKED;

struct hvdxg_command_opensyncobject_return {
    struct hvdxg_d3dkmthandle sync_object;
    struct hvdxg_ntstatus status;
    uint64 gpu_virtual_address;
    uint64 guest_cpu_physical_address;
} __PACKED;

struct hvdxg_command_destroysyncobject {
    struct hvdxg_command_vm_to_host hdr;
    uint32 alignment_padding;
    struct hvdxg_d3dkmthandle sync_object;
} __PACKED;

struct hvdxg_command_signalsyncobject {
    struct hvdxg_command_vgpu_to_host hdr;
    uint32 object_count;
    struct d3dddicb_signalflags flags;
    uint32 context_count;
    uint32 reserved0;
    uint64 fence_value;
    union {
        uint64 cpu_event_handle;
        struct hvdxg_d3dkmthandle device;
    } u;
} __PACKED;

struct hvdxg_command_waitsyncobjectfromcpu {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    uint32 object_count;
    struct d3dddi_waitforsynchronizationobjectfromcpu_flags flags;
    uint32 reserved0;
    uint64 guest_event_pointer;
    uint8 dereference_event;
    uint8 reserved1[7];
} __PACKED;

struct hvdxg_command_waitsyncobjectfromgpu {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle context;
    uint32 object_count;
    uint8 legacy_fence_object;
    uint8 reserved0[7];
    uint64 fence_values[1];
} __PACKED;

struct hvdxg_command_queryvideomemoryinfo {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle adapter;
    uint32 memory_segment_group;
    uint32 physical_adapter_index;
} __PACKED;

struct hvdxg_command_queryvideomemoryinfo_return {
    uint64 budget;
    uint64 current_usage;
    uint64 current_reservation;
    uint64 available_for_reservation;
} __PACKED;

struct hvdxg_command_changevideomemoryreservation {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_changevideomemoryreservation args;
} __PACKED;

struct hvdxg_tracked_allocation {
    uint32 device;
    uint32 resource;
    uint32 allocation;
    uint32 flags;
    uint32 lock_refcount;
    uint32 sysmem_page_count;
    uint64 size;
    uint64 cpu_va;
    uint64 map_size;
    uint64 sysmem;
    uint64 *sysmem_pages;
    void *cpu_vm;
};

struct hvdxg_tracked_resource {
    uint32 device;
    uint32 resource;
    uint32 global_share;
    uint32 allocation_count;
    uint8 create_shared;
    uint8 nt_security_sharing;
    uint32 private_runtime_data_size;
    uint32 resource_priv_drv_data_size;
    uint32 total_priv_drv_data_size;
    uint8 *private_runtime_data;
    uint8 *resource_priv_drv_data;
    uint8 *total_priv_drv_data;
    uint32 alloc_priv_sizes[HV_DXG_ALLOCATION_MAX];
    uint64 allocation_sizes[HV_DXG_ALLOCATION_MAX];
    uint32 allocation_flags[HV_DXG_ALLOCATION_MAX];
};

struct hvdxg_openallocationinfo2 {
    struct hvdxg_d3dkmthandle allocation;
    uint64 priv_drv_data;
    uint32 priv_drv_data_size;
    uint64 gpu_va;
    uint64 reserved[6];
} __PACKED;

enum hvdxg_shared_object_kind {
    HV_DXG_SHARED_OBJECT_SYNC = 1,
    HV_DXG_SHARED_OBJECT_RESOURCE = 2,
};

struct hvdxg_shared_object {
    uint32 kind;
    uint32 device;
    uint32 object;
    uint32 global_share;
    uint32 host_nt_handle;
    uint64 nt_handle;
    uint32 sync_type;
    struct hvdxg_tracked_resource resource;
};

struct hvdxg_tracked_gpuva {
    uint32 adapter;
    uint64 base;
    uint64 size;
    uint64 fence_value;
    uint64 fence_cpu_pa;
};

struct hvdxg_tracked_hwqueue {
    uint32 queue;
    uint32 sync_object;
};

struct hvdxg_tracked_pagingqueue {
    uint32 device;
    uint32 queue;
    uint32 sync_object;
    uint64 fence_pa;
};

struct hvdxg_tracked_sync {
    uint32 sync;
    uint32 type;
    uint32 device;
    uint32 flags;
    uint32 global_shared;
    uint64 fence_cpu_va;
    uint64 fence_kva;
};

struct hvdxg_open_state {
    size_t read_offset;
    int read_emitted;
    char *read_status;
    size_t read_status_len;
    struct hvdxg_d3dkmthandle dxg_process;
    uint32 dxg_process_created;
    uint32 *devices;
    uint32 *contexts;
    struct hvdxg_tracked_hwqueue *hwqueues;
    struct hvdxg_tracked_pagingqueue *paging_queues;
    struct hvdxg_tracked_sync *sync_objects;
    struct hvdxg_tracked_allocation *allocations;
    struct hvdxg_tracked_resource *resources;
    struct hvdxg_tracked_gpuva *gpuvas;
    uint32 device_count;
    uint32 device_capacity;
    uint32 context_count;
    uint32 context_capacity;
    uint32 hwqueue_count;
    uint32 hwqueue_capacity;
    uint32 paging_queue_count;
    uint32 paging_queue_capacity;
    uint32 sync_object_count;
    uint32 sync_object_capacity;
    uint32 allocation_count;
    uint32 allocation_capacity;
    uint32 resource_count;
    uint32 resource_capacity;
    uint32 gpuva_count;
    uint32 gpuva_capacity;
};

struct hvdxg_internal_adapter_info_return {
    uint32 device_types;
    uint32 driver_store_copy_mode;
    uint32 driver_ddi_version;
    uint32 flags;
    struct hvdxg_winluid host_adapter_luid;
    uint16 device_description[80];
    uint16 device_instance_id[260];
    struct hvdxg_winluid host_vgpu_luid;
} __PACKED;

struct vmscsi_request {
    uint16 length;
    uint8 srb_status;
    uint8 scsi_status;
    uint8 port_number;
    uint8 path_id;
    uint8 target_id;
    uint8 lun;
    uint8 cdb_len;
    uint8 sense_info_length;
    uint8 data_in;
    uint8 reserved;
    uint32 data_transfer_length;
    union {
        uint8 cdb[16];
        uint8 sense_data[18];
        uint8 reserved_array[20];
    };
    uint16 reserve;
    uint8 queue_tag;
    uint8 queue_action;
    uint32 srb_flags;
    uint32 time_out_value;
    uint32 queue_sort_key;
} __PACKED;

struct vmstorage_channel_properties {
    uint32 reserved;
    uint16 max_channel_cnt;
    uint16 reserved1;
    uint32 flags;
    uint32 max_transfer_bytes;
    uint64 reserved2;
} __PACKED;

struct vmstorage_protocol_version {
    uint16 major_minor;
    uint16 revision;
} __PACKED;

struct vstor_packet {
    uint32 operation;
    uint32 flags;
    uint32 status;
    union {
        struct vmscsi_request vm_srb;
        struct vmstorage_channel_properties storage_channel_properties;
        struct vmstorage_protocol_version version;
        uint8 buffer[0x34];
    };
} __PACKED;

struct nvsp_message_header {
    uint32 msg_type;
} __PACKED;

struct nvsp_message_init {
    uint32 min_protocol_ver;
    uint32 max_protocol_ver;
} __PACKED;

struct nvsp_message_init_complete {
    uint32 negotiated_protocol_ver;
    uint32 max_mdl_chain_len;
    uint32 status;
} __PACKED;

struct nvsp_1_message_send_ndis_version {
    uint32 ndis_major_ver;
    uint32 ndis_minor_ver;
} __PACKED;

struct nvsp_1_message_send_receive_buffer {
    uint32 gpadl_handle;
    uint16 id;
} __PACKED;

struct nvsp_1_receive_buffer_section {
    uint32 offset;
    uint32 sub_alloc_size;
    uint32 num_sub_allocs;
    uint32 end_offset;
} __PACKED;

struct nvsp_1_message_send_receive_buffer_complete {
    uint32 status;
    uint32 num_sections;
    struct nvsp_1_receive_buffer_section sections[2];
} __PACKED;

struct nvsp_1_message_send_send_buffer {
    uint32 gpadl_handle;
    uint16 id;
} __PACKED;

struct nvsp_1_message_send_send_buffer_complete {
    uint32 status;
    uint32 section_size;
} __PACKED;

struct nvsp_1_message_send_rndis_packet {
    uint32 channel_type;
    uint32 send_buf_section_index;
    uint32 send_buf_section_size;
} __PACKED;

struct nvsp_1_message_send_rndis_packet_complete {
    uint32 status;
} __PACKED;

struct nvsp_2_vsc_capability {
    uint64 data;
} __PACKED;

struct nvsp_2_send_ndis_config {
    uint32 mtu;
    uint32 reserved;
    struct nvsp_2_vsc_capability capability;
} __PACKED;

struct nvsp_message {
    struct nvsp_message_header hdr;
    union {
        struct nvsp_message_init init;
        struct nvsp_message_init_complete init_complete;
        union {
            struct nvsp_1_message_send_ndis_version send_ndis_ver;
            struct nvsp_1_message_send_receive_buffer send_recv_buf;
            struct nvsp_1_message_send_receive_buffer_complete send_recv_buf_complete;
            struct nvsp_1_message_send_send_buffer send_send_buf;
            struct nvsp_1_message_send_send_buffer_complete send_send_buf_complete;
            struct nvsp_1_message_send_rndis_packet send_rndis_pkt;
            struct nvsp_1_message_send_rndis_packet_complete send_rndis_pkt_complete;
        } v1;
        union {
            struct nvsp_2_send_ndis_config send_ndis_config;
        } v2;
        uint8 raw[256];
    } msg;
} __PACKED;

struct rndis_message_header {
    uint32 msg_type;
    uint32 msg_len;
} __PACKED;

struct rndis_initialize_request {
    uint32 req_id;
    uint32 major_ver;
    uint32 minor_ver;
    uint32 max_xfer_size;
} __PACKED;

struct rndis_initialize_complete {
    uint32 req_id;
    uint32 status;
    uint32 major_ver;
    uint32 minor_ver;
    uint32 dev_flags;
    uint32 medium;
    uint32 max_pkt_per_msg;
    uint32 max_xfer_size;
    uint32 pkt_alignment_factor;
    uint32 af_list_offset;
    uint32 af_list_size;
} __PACKED;

struct rndis_query_request {
    uint32 req_id;
    uint32 oid;
    uint32 info_buflen;
    uint32 info_buf_offset;
    uint32 dev_vc_handle;
} __PACKED;

struct rndis_query_complete {
    uint32 req_id;
    uint32 status;
    uint32 info_buflen;
    uint32 info_buf_offset;
} __PACKED;

struct rndis_set_request {
    uint32 req_id;
    uint32 oid;
    uint32 info_buflen;
    uint32 info_buf_offset;
    uint32 dev_vc_handle;
} __PACKED;

struct rndis_set_complete {
    uint32 req_id;
    uint32 status;
} __PACKED;

struct rndis_indicate_status {
    uint32 status;
    uint32 status_buflen;
    uint32 status_buf_offset;
} __PACKED;

struct rndis_packet {
    uint32 data_offset;
    uint32 data_len;
    uint32 oob_data_offset;
    uint32 oob_data_len;
    uint32 num_oob_data_elements;
    uint32 per_pkt_info_offset;
    uint32 per_pkt_info_len;
    uint32 vc_handle;
    uint32 reserved;
} __PACKED;

struct pipe_msg {
    uint32 type;
    uint32 size;
    uint8 data[];
} __PACKED;

struct synthhid_msg_hdr {
    uint32 type;
    uint32 size;
} __PACKED;

struct synthhid_protocol_request {
    struct synthhid_msg_hdr header;
    uint32 version;
} __PACKED;

struct synthhid_protocol_response {
    struct synthhid_msg_hdr header;
    uint32 version;
    uint8 approved;
} __PACKED;

struct synthhid_device_info_ack {
    struct synthhid_msg_hdr header;
    uint8 reserved;
} __PACKED;

struct synthhid_input_report {
    struct synthhid_msg_hdr header;
    uint8 buffer[];
} __PACKED;

struct mousevsc_msg {
    uint32 type;
    uint32 size;
    union {
        struct synthhid_protocol_request request;
        struct synthhid_device_info_ack ack;
    };
} __PACKED;

struct synthkbd_protocol_request {
    uint32 type;
    uint32 version;
} __PACKED;

struct synthkbd_protocol_response {
    uint32 type;
    uint32 status;
} __PACKED;

struct synthkbd_keystroke {
    uint32 type;
    uint16 make_code;
    uint16 reserved;
    uint32 info;
} __PACKED;

struct synthvid_msg_hdr {
    uint32 type;
    uint32 size;
} __PACKED;

struct synthvid_version_req {
    uint32 version;
} __PACKED;

struct synthvid_version_resp {
    uint32 version;
    uint8 accepted;
    uint8 max_outputs;
} __PACKED;

struct synthvid_vram_location {
    uint64 user_ctx;
    uint8 is_vram_gpa_specified;
    uint64 vram_gpa;
} __PACKED;

struct synthvid_vram_location_ack {
    uint64 user_ctx;
} __PACKED;

struct synthvid_screen_info {
    uint16 width;
    uint16 height;
} __PACKED;

struct synthvid_resolution_req {
    uint8 maximum_resolution_count;
} __PACKED;

struct synthvid_resolution_resp {
    uint8 edid[SYNTHVID_EDID_BLOCK_SIZE];
    uint8 resolution_count;
    uint8 default_resolution_index;
    uint8 is_standard;
    struct synthvid_screen_info supported[SYNTHVID_MAX_RESOLUTION_COUNT];
} __PACKED;

struct synthvid_situation {
    uint64 user_ctx;
    uint8 output_count;
    uint8 active;
    uint32 vram_offset;
    uint8 depth_bits;
    uint32 width_pixels;
    uint32 height_pixels;
    uint32 pitch_bytes;
} __PACKED;

struct synthvid_feature_change {
    uint8 dirt_needed;
    uint8 ptr_pos_needed;
    uint8 ptr_shape_needed;
    uint8 situ_needed;
} __PACKED;

struct synthvid_rect {
    int32 x1;
    int32 y1;
    int32 x2;
    int32 y2;
} __PACKED;

struct synthvid_dirt {
    uint8 output;
    uint8 count;
    struct synthvid_rect rect[1];
} __PACKED;

struct synthvid_msg {
    uint32 pipe_type;
    uint32 pipe_size;
    struct synthvid_msg_hdr hdr;
    union {
        struct synthvid_version_req version_req;
        struct synthvid_version_resp version_resp;
        struct synthvid_vram_location vram;
        struct synthvid_vram_location_ack vram_ack;
        struct synthvid_resolution_req resolution_req;
        struct synthvid_resolution_resp resolution_resp;
        struct synthvid_situation situation;
        struct synthvid_feature_change feature;
        struct synthvid_dirt dirt;
        uint8 raw[512];
    };
} __PACKED;

struct hv_hid_field {
    int valid;
    uint16 bit;
    uint8 size;
    int logical_min;
    int logical_max;
    int relative;
};

struct hv_hid_parser {
    uint8 report_id;
    uint16 bitpos;
    struct hv_hid_field x;
    struct hv_hid_field y;
    struct hv_hid_field wheel;
    struct hv_hid_field buttons[3];
};

static struct {
    int present;
    int connected;
    int protocol_ok;
    int device_info_ok;
    int all_offers;
    int monitor_allocated;
    int dedicated;
    int gpadl_ok;
    int open_ok;
    uint32 msg_conn_id;
    uint32 child_relid;
    uint32 signal_conn_id;
    uint8 monitorid;
    uint32 gpadl_status;
    uint32 open_status;
    uint64 hypercall_pa;
    void *hypercall_page;
    uint64 post_pa;
    void *post_page;
    uint64 msg_pa;
    struct hv_message *msg_page;
    uint64 event_pa;
    void *event_page;
    uint64 int_pa;
    void *int_page;
    void *send_int_page;
    void *recv_int_page;
    uint64 monitor1_pa;
    void *monitor1;
    uint64 monitor2_pa;
    void *monitor2;
    uint64 ring_pa;
    uint8 *ring;
    struct hv_ring_buffer *out_ring;
    struct hv_ring_buffer *in_ring;
    struct hv_hid_parser hid;
    uint64 reports;
    uint64 input_packets;
    uint64 ignored_reports;
} hv;

static struct {
    int present;
    int protocol_ok;
    int gpadl_ok;
    int open_ok;
    int monitor_allocated;
    int dedicated;
    uint32 child_relid;
    uint32 signal_conn_id;
    uint8 monitorid;
    uint32 gpadl_status;
    uint32 open_status;
    uint64 ring_pa;
    uint8 *ring;
    struct hv_ring_buffer *out_ring;
    struct hv_ring_buffer *in_ring;
    uint64 events;
} hvkbd;

static struct {
    int present;
    int is_ide;
    int gpadl_ok;
    int open_ok;
    int protocol_ok;
    int initialized;
    int completion_pending;
    int monitor_allocated;
    int dedicated;
    uint32 child_relid;
    uint32 signal_conn_id;
    uint8 monitorid;
    uint32 gpadl_status;
    uint32 open_status;
    uint64 ring_pa;
    uint8 *ring;
    struct hv_ring_buffer *out_ring;
    struct hv_ring_buffer *in_ring;
    struct vstor_packet completion;
    uint64 completion_trans_id;
    uint64 waiting_trans_id;
    uint64 next_trans_id;
    uint64 stale_completions;
    uint64 io_timeouts;
    uint32 path_id;
    uint32 target_id;
    uint32 port;
    uint32 max_transfer_bytes;
    uint64 sectors;
    uint32 sector_size;
    int flush_disabled;
    int flush_warned;
    mutex_t io_lock;
    blkdev_t blkdev;
} hvstor = {
    .blkdev = {
        .dev = {
            .major = 2,
            .minor = 1,
            .devname = "disk0",
            .devmode = S_IFBLK | 0600,
        },
        .readable = 1,
        .writable = 1,
        .block_shift = 0,
    },
};

static struct {
    int present;
    int gpadl_ok;
    int open_ok;
    int initialized;
    int recv_gpadl_ok;
    int send_gpadl_ok;
    int response_pending;
    int rndis_response_pending;
    int rndis_send_done;
    int monitor_allocated;
    int dedicated;
    uint32 child_relid;
    uint32 signal_conn_id;
    uint8 monitorid;
    uint32 gpadl_status;
    uint32 open_status;
    uint32 recv_gpadl_status;
    uint32 send_gpadl_status;
    uint32 nvsp_version;
    uint32 rndis_req_id;
    uint32 rndis_response_type;
    uint32 rndis_response_req_id;
    uint32 rndis_send_status;
    uint64 rndis_send_trans_id;
    uint64 ring_pa;
    uint8 *ring;
    struct hv_ring_buffer *out_ring;
    struct hv_ring_buffer *in_ring;
    uint64 recv_buf_pa;
    uint8 *recv_buf;
    uint32 recv_buf_size;
    uint32 recv_section_size;
    uint64 send_buf_pa;
    uint8 *send_buf;
    uint32 send_buf_size;
    uint32 send_section_size;
    uint32 send_section_count;
    uint8 send_section_busy[HV_NET_MAX_SEND_SECTIONS];
    uint32 tx_inflight;
    struct nvsp_message response;
    uint8 rndis_response[512];
    uint32 rndis_response_len;
    mutex_t tx_lock;
    struct netdev ndev;
    struct netdev_ops ops;
    uint64 rx_packets;
    uint64 tx_packets;
    uint64 rx_drops;
    uint32 debug_rx_count;
    uint32 debug_tx_count;
} hvnet;

static struct {
    int present;
    int gpadl_ok;
    int open_ok;
    int initialized;
    int response_pending;
    int dirt_needed;
    int monitor_allocated;
    int dedicated;
    uint32 child_relid;
    uint32 signal_conn_id;
    uint8 monitorid;
    uint32 gpadl_status;
    uint32 open_status;
    uint64 ring_pa;
    uint8 *ring;
    struct hv_ring_buffer *out_ring;
    struct hv_ring_buffer *in_ring;
    struct synthvid_msg response;
    uint32 debug_rx_count;
    uint32 debug_tx_count;
    uint32 dirty_pending;
    uint32 dirty_x1;
    uint32 dirty_y1;
    uint32 dirty_x2;
    uint32 dirty_y2;
    uint64 dirty_last_ms;
} hvvideo;

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
    uint32 global_gpadl_status;
    uint32 vgpu_gpadl_status;
    uint32 global_open_status;
    uint32 vgpu_open_status;
    uint32 global_mmio_megabytes;
    int32 iospace_last_ret;
    uint32 iospace_last_len;
    int iospace_set;
    uint64 iospace_base;
    uint64 iospace_size;
    uint32 global_rx_packets;
    uint32 vgpu_rx_packets;
    uint8 global_monitorid;
    uint8 vgpu_monitorid;
    uint64 next_trans_id;
    uint64 waiting_trans_id;
    uint64 completion_trans_id;
    volatile int completion_pending;
    uint16 completion_type;
    uint32 completion_len;
    uint8 completion_buf[HV_DXG_RESULT_BYTES];
    uint8 rx_buf[HV_DXG_PACKET_BYTES];
    volatile int pump_active;
    uint32 pump_skips;
    volatile int sync_active;
    uint32 sync_waits;
    uint32 sync_timeouts;
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
    uint32 probe_open_host_version;
    uint32 probe_open_host_compat;
    uint32 probe_info_len;
    uint32 probe_info_flags;
    uint32 probe_async_msg_enabled;
    int d3dkmt_ready;
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
    uint32 syncobject_last_len;
    int32 syncobject_last_ret;
    uint32 syncobject_last_handle;
    uint64 syncobject_last_fence_cpu;
    uint64 syncobject_last_fence_gpu;
    uint64 syncobject_last_fence_pa;
    uint64 syncobject_last_fence_off;
    uint32 syncsignal_last_len;
    int32 syncsignal_last_ret;
    int32 syncsignal_last_status;
    uint32 syncwait_last_len;
    int32 syncwait_last_ret;
    int32 syncwait_last_status;
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
    uint32 last_device_handle;
    uint32 last_resource_handle;
    uint32 last_allocation_handle;
    uint32 last_allocation_device;
    uint64 last_allocation_size;
    uint32 allocation_last_len;
    int32 allocation_last_ret;
    uint32 allocation_last_count;
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
    uint32 destroyalloc_last_len;
    int32 destroyalloc_last_ret;
    uint32 createalloc_unwind_attempts;
    uint32 createalloc_unwind_successes;
    int32 createalloc_unwind_last_ret;
    uint32 makeresident_last_len;
    int32 makeresident_last_ret;
    uint64 makeresident_last_fence;
    uint64 makeresident_last_trim;
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
    uint32 mapgpuva_history_index;
    uint32 mapgpuva_history_len[HV_DXG_RESOURCE_HISTORY_MAX];
    int32 mapgpuva_history_ret[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 mapgpuva_history_status[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 mapgpuva_history_paging_queue[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 mapgpuva_history_allocation[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_pages[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_va[HV_DXG_RESOURCE_HISTORY_MAX];
    uint64 mapgpuva_history_fence[HV_DXG_RESOURCE_HISTORY_MAX];
    uint32 queryadapter_last_type;
    uint32 queryadapter_last_size;
    uint32 queryadapter_last_len;
    int32 queryadapter_last_ret;
    int32 queryadapter_last_status;
    uint32 adapter_vendor_id;
    uint32 adapter_device_id;
    uint32 queryadapter_history_index;
    uint32 queryadapter_history_type[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_size[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_len[HV_DXG_QUERY_HISTORY_MAX];
    int32 queryadapter_history_ret[HV_DXG_QUERY_HISTORY_MAX];
    uint32 queryadapter_history_status[HV_DXG_QUERY_HISTORY_MAX];
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
    int32 shareobject_last_ret;
    int32 shareobject_last_status;
    uint32 shareobject_last_device;
    uint32 shareobject_last_object;
    uint64 shareobject_last_nt_handle;
    uint32 ntshared_last_create_len;
    int32 ntshared_last_create_ret;
    uint32 ntshared_last_create_object;
    uint32 ntshared_last_create_handle;
    uint32 ntshared_last_create_raw0;
    uint32 ntshared_last_destroy_len;
    int32 ntshared_last_destroy_ret;
    uint32 ntshared_last_destroy_handle;
    uint32 sharedhandle_last_cmd;
    int32 sharedhandle_last_ret;
    uint32 sharedhandle_last_device;
    uint32 sharedhandle_last_object;
    uint64 sharedhandle_last_nt_handle;
    uint32 sharedhandle_last_count;
    uint32 unsupported_last_cmd;
    int32 unsupported_last_ret;
    uint32 unsupported_last_device;
    uint32 unsupported_last_handle;
    uint32 unsupported_last_count;
    uint32 syncfile_last_cmd;
    int32 syncfile_last_ret;
    uint32 syncfile_last_device;
    uint32 syncfile_last_object;
    uint32 syncfile_last_context;
    uint64 syncfile_last_handle;
    uint64 syncfile_last_fence;
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
    uint8 createcontext_last_priv_head[64];
    uint32 createcontext_fail_len;
    int32 createcontext_fail_ret;
    int32 createcontext_fail_status;
    uint32 createhwqueue_last_len;
    int32 createhwqueue_last_ret;
    int32 createhwqueue_last_status;
    uint32 createhwqueue_last_context;
    uint32 createhwqueue_last_flags;
    uint32 createhwqueue_last_priv_size;
    uint32 createhwqueue_last_priv_head_len;
    uint8 createhwqueue_last_priv_head[64];
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
    uint8 submithwqueue_last_priv_head[32];
    uint32 destroyhwqueue_last_len;
    int32 destroyhwqueue_last_ret;
    struct hvdxg_d3dkmthandle dxg_process;
    uint32 dxg_process_created;
    uint32 host_adapter_handle;
    struct hvdxg_winluid adapter_luid;
    struct hvdxg_winluid host_adapter_luid;
    struct hvdxg_winluid host_vgpu_luid;
    uint32 use_ext_header;
    uint32 pci_dxg_device;
    uint32 pci_dxg_vmbus_version;
    uint32 pci_dxg_guid[4];
    struct hvdxg_winluid pci_host_vgpu_luid;
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
static int hvdxg_d3dkmt_ensure(void);
static int hvdxg_luid_equal(struct hvdxg_winluid a, struct hvdxg_winluid b);
static struct hvdxg_winluid hvdxg_luid_from_guid(const struct hv_guid *guid);
static int hvdxg_ntstatus_to_errno(struct hvdxg_ntstatus status);
static int hvdxg_send_sync_vgpu(const void *cmd, uint32 cmd_len,
                                void *result, uint32 result_len,
                                uint32 *actual_len);
static int hvdxg_send_sync_global(const void *cmd, uint32 cmd_len,
                                  void *result, uint32 result_len,
                                  uint32 *actual_len);
static int hvdxg_probe_transport(void);
static uint64 hvdxg_alloc_host_event(void);
static uint64 hvdxg_alloc_host_event_file(struct vfs_file *file,
                                          int remove_after_signal);
static void hvdxg_remove_host_event(uint64 id);
static void hvdxg_pump_events_ms(uint64 timeout_ms);
static int hvdxg_wait_host_event(uint64 event_id, uint64 timeout_ms);
static int hvdxg_send_waitsyncobjectfromcpu(
    struct d3dkmt_waitforsynchronizationobjectfromcpu *req,
    const void *objects, const void *fence_values, uint64 event_id,
    uint32 object_size, uint32 fence_size, uint32 *actual_len);

static int hvdxg_utf16_ascii_equals(const uint16 *value, const char *ascii)
{
    uint32 i = 0;

    while (ascii[i] != '\0') {
        if (value[i] != (uint16)ascii[i])
            return 0;
        i++;
    }
    return value[i] == 0;
}

static void hvdxg_utf16_to_ascii(char *dst, uint32 dst_size,
                                 const uint16 *value)
{
    uint32 i = 0;

    if (dst_size == 0)
        return;
    while (i + 1 < dst_size && value[i] != 0) {
        uint16 ch = value[i];

        dst[i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '?';
        i++;
    }
    dst[i] = 0;
}

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

void hyperv_dxg_note_pci(uint32 device, uint32 guid0, uint32 guid1,
                         uint32 guid2, uint32 guid3, uint32 vmbus_version,
                         uint32 luid_low, uint32 luid_high)
{
    hvdxg.pci_dxg_device = device;
    hvdxg.pci_dxg_vmbus_version = vmbus_version;
    hvdxg.pci_dxg_guid[0] = guid0;
    hvdxg.pci_dxg_guid[1] = guid1;
    hvdxg.pci_dxg_guid[2] = guid2;
    hvdxg.pci_dxg_guid[3] = guid3;
    hvdxg.pci_host_vgpu_luid.a = luid_low;
    hvdxg.pci_host_vgpu_luid.b = luid_high;
    if (luid_low != 0 || luid_high != 0)
        hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
}

static uint32 hvdxg_utf16_multisz_size(const char *const *strings,
                                       uint32 count)
{
    uint32 chars = 1;

    for (uint32 i = 0; i < count; i++) {
        const char *s = strings[i];

        while (*s++ != '\0')
            chars++;
        chars++;
    }
    return chars * sizeof(uint16);
}

static void hvdxg_write_utf16_multisz(uint8 *dst,
                                      const char *const *strings,
                                      uint32 count)
{
    uint16 *out = (uint16 *)dst;

    for (uint32 i = 0; i < count; i++) {
        const char *s = strings[i];

        while (*s != '\0')
            *out++ = (uint16)*s++;
        *out++ = 0;
    }
    *out++ = 0;
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

static void hvdxg_note_queryadapter_history(uint32 type, uint32 size,
                                            int32 ret, uint32 status)
{
    uint32 slot = hvdxg.queryadapter_history_index %
                  HV_DXG_QUERY_HISTORY_MAX;

    hvdxg.queryadapter_history_type[slot] = type;
    hvdxg.queryadapter_history_size[slot] = size;
    hvdxg.queryadapter_history_len[slot] = hvdxg.queryadapter_last_len;
    hvdxg.queryadapter_history_ret[slot] = ret;
    hvdxg.queryadapter_history_status[slot] = status;
    hvdxg.queryadapter_history_index++;
}

static uint32 hvdxg_read_u32(const void *ptr)
{
    uint32 value;

    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void hvdxg_note_adapter_hardware_id(const uint8 *private_data,
                                           uint32 private_data_size)
{
    if (private_data_size < 12 || private_data == NULL)
        return;

    hvdxg.adapter_vendor_id = hvdxg_read_u32(private_data + 4);
    hvdxg.adapter_device_id = hvdxg_read_u32(private_data + 8);
}

static void hvdxg_note_allocation_history(uint32 len, int32 ret,
                                          uint32 device, uint32 resource,
                                          uint32 allocation, uint64 size,
                                          uint32 count, uint32 priv_size)
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
    hvdxg.allocation_history_index++;
}

static void hvdxg_note_mapgpuva_history(uint32 len, int32 ret, uint32 status,
                                        uint32 paging_queue,
                                        uint32 allocation, uint64 pages,
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
    hvdxg.mapgpuva_history_va[slot] = va;
    hvdxg.mapgpuva_history_fence[slot] = fence;
    hvdxg.mapgpuva_history_index++;
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

static void hvdxg_status_append_hex(char *status, size_t status_size,
                                    int *len, const char *prefix,
                                    const uint8 *data, uint32 data_len)
{
    uint32 shown = data_len < 64 ? data_len : 64;

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
        top_nr[1], count[1], avg_us[1], max_us[1], last_us[1],
        top_nr[2], count[2], avg_us[2], max_us[2], last_us[2],
        top_nr[3], count[3], avg_us[3], max_us[3], last_us[3]);
}

#define HV_DXG_IOCTL_H_ARGS(i) \
    hvdxg.ioctl_history_cmd[(i)], \
    hvdxg.ioctl_history_nr[(i)], \
    hvdxg.ioctl_history_ret[(i)]

static int hvdxg_open(cdev_t *cdev)
{
    (void)cdev;
    hvdxg.read_emitted = 0;
    hvdxg.read_offset = 0;
    hvdxg.open_count++;
    hvdxg.live_open_count++;
    return 0;
}

static int hvdxg_release(cdev_t *cdev)
{
    (void)cdev;
    if (hvdxg.live_open_count > 0)
        hvdxg.live_open_count--;
    return 0;
}

static int hvdxg_read_status(cdev_t *cdev, bool user, void *buf,
                             size_t count, size_t *read_offset,
                             int *read_emitted, char **read_status,
                             size_t *read_status_len)
{
    (void)cdev;
    char *status;
    size_t status_size = HV_DXG_STATUS_BUF_SIZE;
    int cached = 0;
    int len = 0;
    uint32 ioctl_top_nr[HV_DXG_IOCTL_TIME_TOP];

    if (hvdxg.vgpu_open_ok && hvdxg.probe_attempts == 0)
        (void)hvdxg_probe_transport();

    if (read_offset == NULL || read_emitted == NULL)
        return -EINVAL;
    if (read_status != NULL && read_status_len != NULL &&
        *read_status != NULL) {
        status = *read_status;
        cached = 1;
        goto copy_cached;
    }

    status = kvmalloc(status_size);
    if (status == NULL)
        return -ENOMEM;
    hvdxg_ioctl_timing_top(ioctl_top_nr);

    len = snprintf(status, status_size,
        "hyperv_dxg_global=%d relid=%u conn=%u monitor=%u\n"
        "hyperv_dxg_global_transport=gpadl:%d status:%u open:%d status:%u rx:%u\n"
        "dxg_iospace=offer_mb:%u set:%d ret:%d len:%u base:0x%lx size:0x%lx\n"
        "hyperv_dxg_vgpu=%d count=%d relid=%u conn=%u monitor=%u\n"
        "hyperv_dxg_vgpu_transport=gpadl:%d status:%u open:%d status:%u rx:%u\n"
        "dxg_status=size:%u truncated:%d snapshot:%d\n"
        "dxg_host_events=next:%lu last:%lu signals:%u wait_ok:%u wait_timeout:%u wait_fail:%u\n"
        "dxg_channel_pump=active:%d skips:%u sync_active:%d sync_waits:%u sync_timeouts:%u\n"
        "dxg_track_limits=limit:%u alloc_max:%u alloc_drop:%u gpuva_max:%u gpuva_drop:%u hwqueue_max:%u hwqueue_drop:%u pagingqueue_max:%u pagingqueue_drop:%u\n"
        "dxg_pagingqueue_last=len:%u ret:%d queue:0x%x sync:0x%x fence_pa:0x%lx fence_off:0x%lx\n"
        "dxg_gpuva_last=reserve_len:%u reserve_ret:%d va:0x%lx fence:%lu free_len:%u free_ret:%d\n"
        "dxg_syncobject_last=len:%u ret:%d handle:0x%x fence_cpu:0x%lx fence_gpu:0x%lx fence_pa:0x%lx fence_off:0x%lx signal_len:%u signal_ret:%d signal_status:0x%x wait_len:%u wait_ret:%d wait_status:0x%x gpu_signal_len:%u gpu_signal_ret:%d gpu_signal_status:0x%x gpu_wait_len:%u gpu_wait_ret:%d gpu_wait_status:0x%x\n"
        "dxg_syncgpu_wait_detail=context:0x%x object:0x%x count:%u type:%u legacy:%u fence:%lu cmd_len:%u\n"
        "dxg_allocation_last=len:%u ret:%d count:%u resource:0x%x allocation:0x%x size:%lu destroy_len:%u destroy_ret:%d unwind_attempts:%u unwind_successes:%u unwind_ret:%d\n"
        "dxg_allocation_priv=size:%u flags:0x%x sysmem:0x%lx pri:0x%lx existing_pages:%u pin_ret:%d set_ret:%d pin_ok:%u set_ok:%u total_pages:%lu in_len:%u in:%02x%02x%02x%02x%02x%02x%02x%02x out_len:%u out:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_residency_last=make_len:%u make_ret:%d fence:%lu trim:%lu evict_len:%u evict_ret:%d evict_trim:%lu\n"
        "dxg_mapgpuva_last=len:%u ret:%d status:0x%x pq:0x%x alloc:0x%x base:0x%lx min:0x%lx max:0x%lx pages:%lu prot:0x%lx dprot:0x%lx va:0x%lx fence:%lu\n"
        "dxg_submit_last=submit_len:%u submit_ret:%d submit_status:0x%x cmd:0x%lx cmd_len:%u flags:0x%x priv:%u contexts:%u ctx0:0x%x\n"
        "dxg_lock2_last=len:%u ret:%d status:0x%x allocation:0x%x offset:0x%lx user_va:0x%lx unlock_len:%u unlock_ret:%d unlock_status:0x%x unlock_allocation:0x%x\n"
        "dxg_allocation_history=index:%u h0:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h1:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h2:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h3:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u\n"
        "dxg_allocation_history2=h4:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h5:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h6:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u h7:dev:0x%x/res:0x%x/alloc:0x%x/size:%lu/len:%u/ret:%d/count:%u/priv:%u\n"
        "dxg_mapgpuva_history=index:%u h0:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h1:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h2:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h3:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x\n"
        "dxg_mapgpuva_history2=h4:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h5:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h6:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x h7:alloc:0x%x/va:0x%lx/pages:%lu/fence:%lu/len:%u/ret:%d/status:0x%x/pq:0x%x\n"
        "dxg_lock2_history=index:%u h0:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h1:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h2:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h3:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x\n"
        "dxg_lock2_history2=h4:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h5:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h6:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x h7:dev:0x%x/alloc:0x%x/off:0x%lx/user:0x%lx/size:%lu/len:%u/ret:%d/status:0x%x\n"
        "dxg_queryadapter_last=type:%u size:%u len:%u ret:%d status:0x%x\n"
        "dxg_adapter_hardware=vendor:0x%x device:0x%x\n"
        "dxg_queryadapter_history=index:%u h0:%u/%u/%u/%d/0x%x h1:%u/%u/%u/%d/0x%x h2:%u/%u/%u/%d/0x%x h3:%u/%u/%u/%d/0x%x h4:%u/%u/%u/%d/0x%x h5:%u/%u/%u/%d/0x%x h6:%u/%u/%u/%d/0x%x h7:%u/%u/%u/%d/0x%x\n"
        "dxg_queryadapter_history2=h8:%u/%u/%u/%d/0x%x h9:%u/%u/%u/%d/0x%x h10:%u/%u/%u/%d/0x%x h11:%u/%u/%u/%d/0x%x h12:%u/%u/%u/%d/0x%x h13:%u/%u/%u/%d/0x%x h14:%u/%u/%u/%d/0x%x h15:%u/%u/%u/%d/0x%x\n"
        "dxg_queryregistry_last=query:%u flags:0x%x value_type:%u phys:%u output_size:%u status:%u name:%s name0:0x%x name1:0x%x\n"
        "dxg_feature_last=id:%u len:%u ret:%d status:0x%x result:0x%x\n"
        "dxg_priority_last=sched_len:%u sched_ret:%d sched_status:0x%x context:0x%x priority:%d alloc_len:%u alloc_ret:%d alloc_status:0x%x count:%u residency_len:%u residency_ret:%d residency_status:0x%x residency_count:%u residency_value:%u\n"
        "dxg_statistics_last=len:%u ret:%d status:0x%x type:%u\n"
        "dxg_clockcalibration_last=len:%u ret:%d status:0x%x node:%u gpu_freq:%lu gpu_counter:%lu cpu_counter:%lu\n"
        "dxg_stdalloc_last=len:%u ret:%d status:0x%x alloc_priv:%u res_priv:%u\n"
        "dxg_escape_last=len:%u ret:%d type:%u flags:0x%x size:%u\n"
        "dxg_shareobject_last=len:%u ret:%d status:0x%x device:0x%x object:0x%x nt:0x%lx\n"
        "dxg_ntshared_last=create_len:%u create_ret:%d object:0x%x handle:0x%x raw0:0x%x destroy_len:%u destroy_ret:%d destroy_handle:0x%x\n"
        "dxg_sharedhandle_last=cmd:0x%x ret:%d device:0x%x object:0x%x nt:0x%lx count:%u\n"
        "dxg_unsupported_last=cmd:0x%x ret:%d device:0x%x handle:0x%x count:%u\n"
        "dxg_syncfile_last=cmd:0x%x ret:%d device:0x%x object:0x%x context:0x%x handle:0x%lx fence:%lu\n"
        "dxg_updateallocproperty_last=len:%u ret:%d status:0x%x allocation:0x%x fence:%lu\n"
        "dxg_vidmem_reservation_last=len:%u ret:%d status:0x%x group:%u reservation:%lu\n"
        "dxg_offer_reclaim_last=offer_len:%u offer_ret:%d offer_status:0x%x offer_count:%u reclaim_len:%u reclaim_ret:%d reclaim_status:0x%x reclaim_count:%u reclaim_result0:%u reclaim_fence:%lu\n"
        "dxg_updategpuva_last=len:%u ret:%d status:0x%x ops:%u fence:%lu\n"
        "dxg_cacheops_last=len:%u ret:%d status:0x%x allocation:0x%x\n"
        "dxg_context_last=len:%u ret:%d handle:0x%x device:0x%x node:%u engine:%u flags:0x%x hint:%u priv:%u fail_len:%u fail_ret:%d fail_status:0x%x\n"
        "dxg_context_priv_head=len:%u bytes:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_hwqueue_last=create_len:%u create_ret:%d create_status:0x%x context:0x%x flags:0x%x priv:%u queue:0x%x fence:0x%x fence_cpu:0x%lx fence_gpu:0x%lx submit_len:%u submit_ret:%d destroy_len:%u destroy_ret:%d\n"
        "dxg_hwqueue_priv_head=create_len:%u create:%02x%02x%02x%02x%02x%02x%02x%02x submit_queue:0x%x submit_fence:%lu submit_cmd_len:%u submit_priv:%u submit_len:%u submit:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "dxg_probe_attempts=%u successes=%u last_ret=%d\n"
        "dxg_probe_open_status=%d handle=0x%x host_version=%u host_compat=%u\n"
        "dxg_probe_info_len=%u flags=0x%x async_msg=%u ext_header=%u host_vgpu_luid=%x:%x\n"
        "dxg_pci=device:0x%x version:%u guid:%x-%x-%x-%x host_luid:%x:%x\n"
        "dxg_probe_last_packet=type:%u len:%u prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "d3dkmt_ioctls=%u successes=%u ready=%d last_ret=%d process=0x%x\n"
        "d3dkmt_ioctl_history=index:%u h0:0x%x/%u/%d h1:0x%x/%u/%d h2:0x%x/%u/%d h3:0x%x/%u/%d h4:0x%x/%u/%d h5:0x%x/%u/%d h6:0x%x/%u/%d h7:0x%x/%u/%d\n"
        "d3dkmt_ioctl_history2=h8:0x%x/%u/%d h9:0x%x/%u/%d h10:0x%x/%u/%d h11:0x%x/%u/%d h12:0x%x/%u/%d h13:0x%x/%u/%d h14:0x%x/%u/%d h15:0x%x/%u/%d\n"
        "d3dkmt_open_files=opens:%u live:%u cleanup_attempts:%u cleanup_successes:%u cleanup_last_ret:%d cleanup_last_op:%u cleanup_last_handle:0x%x cleanup_failed_op:%u cleanup_failed_handle:0x%x cleanup_had_tracked:%u\n"
        "note=Hyper-V GPU-PV D3DKMT adapter ioctls are available; "
        "device/context/sync/paging/allocation residency/map/submit ioctls are wired; higher-level OpenGL/D3D runtime validation is still pending.\n",
        hvdxg.global_present, hvdxg.global_relid, hvdxg.global_conn_id,
        hvdxg.global_monitorid, hvdxg.global_gpadl_ok,
        hvdxg.global_gpadl_status, hvdxg.global_open_ok,
        hvdxg.global_open_status, hvdxg.global_rx_packets,
        hvdxg.global_mmio_megabytes, hvdxg.iospace_set,
        hvdxg.iospace_last_ret, hvdxg.iospace_last_len,
        hvdxg.iospace_base, hvdxg.iospace_size,
        hvdxg.vgpu_present, hvdxg.vgpu_count, hvdxg.vgpu_relid,
        hvdxg.vgpu_conn_id, hvdxg.vgpu_monitorid, hvdxg.vgpu_gpadl_ok,
        hvdxg.vgpu_gpadl_status, hvdxg.vgpu_open_ok,
        hvdxg.vgpu_open_status, hvdxg.vgpu_rx_packets,
        (uint32)status_size, 0, read_status != NULL ? 1 : 0,
        hvdxg.host_event_next_id, hvdxg.host_event_last_id,
        hvdxg.host_event_signal_count, hvdxg.host_event_wait_successes,
        hvdxg.host_event_wait_timeouts, hvdxg.host_event_wait_failures,
        hvdxg.pump_active, hvdxg.pump_skips, hvdxg.sync_active,
        hvdxg.sync_waits, hvdxg.sync_timeouts,
        (uint32)HV_DXG_OPEN_TRACKED_MAX, hvdxg.track_allocation_max,
        hvdxg.track_allocation_drops, hvdxg.track_gpuva_max,
        hvdxg.track_gpuva_drops, hvdxg.track_hwqueue_max,
        hvdxg.track_hwqueue_drops, hvdxg.track_pagingqueue_max,
        hvdxg.track_pagingqueue_drops,
        hvdxg.pagingqueue_last_len, hvdxg.pagingqueue_last_ret,
        hvdxg.pagingqueue_last_queue, hvdxg.pagingqueue_last_sync,
        hvdxg.pagingqueue_last_fence_pa, hvdxg.pagingqueue_last_fence_off,
        hvdxg.gpuva_reserve_last_len, hvdxg.gpuva_reserve_last_ret,
        hvdxg.gpuva_reserve_last_va, hvdxg.gpuva_reserve_last_fence,
        hvdxg.gpuva_free_last_len, hvdxg.gpuva_free_last_ret,
        hvdxg.syncobject_last_len, hvdxg.syncobject_last_ret,
        hvdxg.syncobject_last_handle, hvdxg.syncobject_last_fence_cpu,
        hvdxg.syncobject_last_fence_gpu, hvdxg.syncobject_last_fence_pa,
        hvdxg.syncobject_last_fence_off, hvdxg.syncsignal_last_len,
        hvdxg.syncsignal_last_ret, (uint32)hvdxg.syncsignal_last_status,
        hvdxg.syncwait_last_len, hvdxg.syncwait_last_ret,
        (uint32)hvdxg.syncwait_last_status,
        hvdxg.syncgpu_signal_last_len, hvdxg.syncgpu_signal_last_ret,
        (uint32)hvdxg.syncgpu_signal_last_status,
        hvdxg.syncgpu_wait_last_len, hvdxg.syncgpu_wait_last_ret,
        (uint32)hvdxg.syncgpu_wait_last_status,
        hvdxg.syncgpu_wait_last_context,
        hvdxg.syncgpu_wait_last_object,
        hvdxg.syncgpu_wait_last_object_count,
        hvdxg.syncgpu_wait_last_object_type,
        hvdxg.syncgpu_wait_last_legacy,
        hvdxg.syncgpu_wait_last_fence,
        hvdxg.syncgpu_wait_last_cmd_len,
        hvdxg.allocation_last_len, hvdxg.allocation_last_ret,
        hvdxg.allocation_last_count, hvdxg.last_resource_handle,
        hvdxg.last_allocation_handle, hvdxg.last_allocation_size,
        hvdxg.destroyalloc_last_len, hvdxg.destroyalloc_last_ret,
        hvdxg.createalloc_unwind_attempts,
        hvdxg.createalloc_unwind_successes,
        hvdxg.createalloc_unwind_last_ret,
        hvdxg.allocation_last_priv_size, hvdxg.allocation_last_flags,
        hvdxg.allocation_last_sysmem, hvdxg.allocation_last_priority,
        hvdxg.existing_sysmem_last_pages,
        hvdxg.existing_sysmem_last_pin_ret,
        hvdxg.existing_sysmem_last_set_ret,
        hvdxg.existing_sysmem_pin_successes,
        hvdxg.existing_sysmem_set_successes,
        hvdxg.existing_sysmem_total_pages,
        hvdxg.allocation_last_in_priv_head_len,
        hvdxg.allocation_last_in_priv_head[0],
        hvdxg.allocation_last_in_priv_head[1],
        hvdxg.allocation_last_in_priv_head[2],
        hvdxg.allocation_last_in_priv_head[3],
        hvdxg.allocation_last_in_priv_head[4],
        hvdxg.allocation_last_in_priv_head[5],
        hvdxg.allocation_last_in_priv_head[6],
        hvdxg.allocation_last_in_priv_head[7],
        hvdxg.allocation_last_out_priv_head_len,
        hvdxg.allocation_last_out_priv_head[0],
        hvdxg.allocation_last_out_priv_head[1],
        hvdxg.allocation_last_out_priv_head[2],
        hvdxg.allocation_last_out_priv_head[3],
        hvdxg.allocation_last_out_priv_head[4],
        hvdxg.allocation_last_out_priv_head[5],
        hvdxg.allocation_last_out_priv_head[6],
        hvdxg.allocation_last_out_priv_head[7],
        hvdxg.makeresident_last_len, hvdxg.makeresident_last_ret,
        hvdxg.makeresident_last_fence, hvdxg.makeresident_last_trim,
        hvdxg.evict_last_len, hvdxg.evict_last_ret,
        hvdxg.evict_last_trim, hvdxg.mapgpuva_last_len,
        hvdxg.mapgpuva_last_ret, (uint32)hvdxg.mapgpuva_last_status,
        hvdxg.mapgpuva_last_paging_queue, hvdxg.mapgpuva_last_allocation,
        hvdxg.mapgpuva_last_base, hvdxg.mapgpuva_last_min,
        hvdxg.mapgpuva_last_max, hvdxg.mapgpuva_last_size_pages,
        hvdxg.mapgpuva_last_protection,
        hvdxg.mapgpuva_last_driver_protection, hvdxg.mapgpuva_last_va,
        hvdxg.mapgpuva_last_fence, hvdxg.submit_last_len,
        hvdxg.submit_last_ret, (uint32)hvdxg.submit_last_status,
        hvdxg.submit_last_command_buffer,
        hvdxg.submit_last_command_length, hvdxg.submit_last_flags,
        hvdxg.submit_last_priv_size, hvdxg.submit_last_context_count,
        hvdxg.submit_last_context0,
        hvdxg.lock2_last_len, hvdxg.lock2_last_ret,
        (uint32)hvdxg.lock2_last_status, hvdxg.lock2_last_allocation,
        hvdxg.lock2_last_offset, hvdxg.lock2_last_user_va,
        hvdxg.unlock2_last_len, hvdxg.unlock2_last_ret,
        (uint32)hvdxg.unlock2_last_status,
        hvdxg.unlock2_last_allocation,
        hvdxg.allocation_history_index,
        HV_DXG_AH_ARGS(0), HV_DXG_AH_ARGS(1),
        HV_DXG_AH_ARGS(2), HV_DXG_AH_ARGS(3),
        HV_DXG_AH_ARGS(4), HV_DXG_AH_ARGS(5),
        HV_DXG_AH_ARGS(6), HV_DXG_AH_ARGS(7),
        hvdxg.mapgpuva_history_index,
        HV_DXG_MH_ARGS(0), HV_DXG_MH_ARGS(1),
        HV_DXG_MH_ARGS(2), HV_DXG_MH_ARGS(3),
        HV_DXG_MH_ARGS(4), HV_DXG_MH_ARGS(5),
        HV_DXG_MH_ARGS(6), HV_DXG_MH_ARGS(7),
        hvdxg.lock2_history_index,
        HV_DXG_LH_ARGS(0), HV_DXG_LH_ARGS(1),
        HV_DXG_LH_ARGS(2), HV_DXG_LH_ARGS(3),
        HV_DXG_LH_ARGS(4), HV_DXG_LH_ARGS(5),
        HV_DXG_LH_ARGS(6), HV_DXG_LH_ARGS(7),
        hvdxg.queryadapter_last_type, hvdxg.queryadapter_last_size,
        hvdxg.queryadapter_last_len, hvdxg.queryadapter_last_ret,
        (uint32)hvdxg.queryadapter_last_status,
        hvdxg.adapter_vendor_id, hvdxg.adapter_device_id,
        hvdxg.queryadapter_history_index,
        HV_DXG_QH_ARGS(0), HV_DXG_QH_ARGS(1),
        HV_DXG_QH_ARGS(2), HV_DXG_QH_ARGS(3),
        HV_DXG_QH_ARGS(4), HV_DXG_QH_ARGS(5),
        HV_DXG_QH_ARGS(6), HV_DXG_QH_ARGS(7),
        HV_DXG_QH_ARGS(8), HV_DXG_QH_ARGS(9),
        HV_DXG_QH_ARGS(10), HV_DXG_QH_ARGS(11),
        HV_DXG_QH_ARGS(12), HV_DXG_QH_ARGS(13),
        HV_DXG_QH_ARGS(14), HV_DXG_QH_ARGS(15),
        hvdxg.queryregistry_last_query_type,
        hvdxg.queryregistry_last_flags,
        hvdxg.queryregistry_last_value_type,
        hvdxg.queryregistry_last_phys,
        hvdxg.queryregistry_last_output_size,
        hvdxg.queryregistry_last_status,
        hvdxg.queryregistry_last_name,
        hvdxg.queryregistry_last_name0,
        hvdxg.queryregistry_last_name1,
        hvdxg.feature_last_id, hvdxg.feature_last_len,
        hvdxg.feature_last_ret, (uint32)hvdxg.feature_last_status,
        hvdxg.feature_last_result,
        hvdxg.schedprio_last_len, hvdxg.schedprio_last_ret,
        (uint32)hvdxg.schedprio_last_status,
        hvdxg.schedprio_last_context, hvdxg.schedprio_last_value,
        hvdxg.allocprio_last_len, hvdxg.allocprio_last_ret,
        (uint32)hvdxg.allocprio_last_status,
        hvdxg.allocprio_last_count,
        hvdxg.allocres_last_len, hvdxg.allocres_last_ret,
        (uint32)hvdxg.allocres_last_status,
        hvdxg.allocres_last_count, hvdxg.allocres_last_value,
        hvdxg.statistics_last_len, hvdxg.statistics_last_ret,
        (uint32)hvdxg.statistics_last_status,
        hvdxg.statistics_last_type,
        hvdxg.clockcalibration_last_len, hvdxg.clockcalibration_last_ret,
        (uint32)hvdxg.clockcalibration_last_status,
        hvdxg.clockcalibration_last_node,
        hvdxg.clockcalibration_last_gpu_frequency,
        hvdxg.clockcalibration_last_gpu_counter,
        hvdxg.clockcalibration_last_cpu_counter,
        hvdxg.stdalloc_last_len, hvdxg.stdalloc_last_ret,
        (uint32)hvdxg.stdalloc_last_status,
        hvdxg.stdalloc_last_alloc_size, hvdxg.stdalloc_last_res_size,
        hvdxg.escape_last_len, hvdxg.escape_last_ret,
        hvdxg.escape_last_type, hvdxg.escape_last_flags,
        hvdxg.escape_last_size,
        hvdxg.shareobject_last_len, hvdxg.shareobject_last_ret,
        (uint32)hvdxg.shareobject_last_status,
        hvdxg.shareobject_last_device, hvdxg.shareobject_last_object,
        hvdxg.shareobject_last_nt_handle,
        hvdxg.ntshared_last_create_len, hvdxg.ntshared_last_create_ret,
        hvdxg.ntshared_last_create_object,
        hvdxg.ntshared_last_create_handle,
        hvdxg.ntshared_last_create_raw0,
        hvdxg.ntshared_last_destroy_len, hvdxg.ntshared_last_destroy_ret,
        hvdxg.ntshared_last_destroy_handle,
        hvdxg.sharedhandle_last_cmd, hvdxg.sharedhandle_last_ret,
        hvdxg.sharedhandle_last_device, hvdxg.sharedhandle_last_object,
        hvdxg.sharedhandle_last_nt_handle, hvdxg.sharedhandle_last_count,
        hvdxg.unsupported_last_cmd, hvdxg.unsupported_last_ret,
        hvdxg.unsupported_last_device, hvdxg.unsupported_last_handle,
        hvdxg.unsupported_last_count,
        hvdxg.syncfile_last_cmd, hvdxg.syncfile_last_ret,
        hvdxg.syncfile_last_device, hvdxg.syncfile_last_object,
        hvdxg.syncfile_last_context, hvdxg.syncfile_last_handle,
        hvdxg.syncfile_last_fence,
        hvdxg.updateallocproperty_last_len,
        hvdxg.updateallocproperty_last_ret,
        (uint32)hvdxg.updateallocproperty_last_status,
        hvdxg.updateallocproperty_last_allocation,
        hvdxg.updateallocproperty_last_fence,
        hvdxg.vidmem_reservation_last_len,
        hvdxg.vidmem_reservation_last_ret,
        (uint32)hvdxg.vidmem_reservation_last_status,
        hvdxg.vidmem_reservation_last_group,
        hvdxg.vidmem_reservation_last_value,
        hvdxg.offer_last_len, hvdxg.offer_last_ret,
        (uint32)hvdxg.offer_last_status, hvdxg.offer_last_count,
        hvdxg.reclaim_last_len, hvdxg.reclaim_last_ret,
        (uint32)hvdxg.reclaim_last_status, hvdxg.reclaim_last_count,
        hvdxg.reclaim_last_result0, hvdxg.reclaim_last_fence,
        hvdxg.updategpuva_last_len, hvdxg.updategpuva_last_ret,
        (uint32)hvdxg.updategpuva_last_status,
        hvdxg.updategpuva_last_ops, hvdxg.updategpuva_last_fence,
        hvdxg.cacheops_last_len, hvdxg.cacheops_last_ret,
        (uint32)hvdxg.cacheops_last_status,
        hvdxg.cacheops_last_allocation,
        hvdxg.createcontext_last_len,
        hvdxg.createcontext_last_ret, hvdxg.createcontext_last_handle,
        hvdxg.createcontext_last_device, hvdxg.createcontext_last_node,
        hvdxg.createcontext_last_engine, hvdxg.createcontext_last_flags,
        hvdxg.createcontext_last_hint, hvdxg.createcontext_last_priv_size,
        hvdxg.createcontext_fail_len, hvdxg.createcontext_fail_ret,
        (uint32)hvdxg.createcontext_fail_status,
        hvdxg.createcontext_last_priv_head_len,
        hvdxg.createcontext_last_priv_head[0],
        hvdxg.createcontext_last_priv_head[1],
        hvdxg.createcontext_last_priv_head[2],
        hvdxg.createcontext_last_priv_head[3],
        hvdxg.createcontext_last_priv_head[4],
        hvdxg.createcontext_last_priv_head[5],
        hvdxg.createcontext_last_priv_head[6],
        hvdxg.createcontext_last_priv_head[7],
        hvdxg.createhwqueue_last_len, hvdxg.createhwqueue_last_ret,
        (uint32)hvdxg.createhwqueue_last_status,
        hvdxg.createhwqueue_last_context, hvdxg.createhwqueue_last_flags,
        hvdxg.createhwqueue_last_priv_size,
        hvdxg.createhwqueue_last_queue, hvdxg.createhwqueue_last_fence,
        hvdxg.createhwqueue_last_fence_cpu,
        hvdxg.createhwqueue_last_fence_gpu, hvdxg.submithwqueue_last_len,
        hvdxg.submithwqueue_last_ret, hvdxg.destroyhwqueue_last_len,
        hvdxg.destroyhwqueue_last_ret,
        hvdxg.createhwqueue_last_priv_head_len,
        hvdxg.createhwqueue_last_priv_head[0],
        hvdxg.createhwqueue_last_priv_head[1],
        hvdxg.createhwqueue_last_priv_head[2],
        hvdxg.createhwqueue_last_priv_head[3],
        hvdxg.createhwqueue_last_priv_head[4],
        hvdxg.createhwqueue_last_priv_head[5],
        hvdxg.createhwqueue_last_priv_head[6],
        hvdxg.createhwqueue_last_priv_head[7],
        hvdxg.submithwqueue_last_queue,
        hvdxg.submithwqueue_last_fence_id,
        hvdxg.submithwqueue_last_command_length,
        hvdxg.submithwqueue_last_priv_size,
        hvdxg.submithwqueue_last_priv_head_len,
        hvdxg.submithwqueue_last_priv_head[0],
        hvdxg.submithwqueue_last_priv_head[1],
        hvdxg.submithwqueue_last_priv_head[2],
        hvdxg.submithwqueue_last_priv_head[3],
        hvdxg.submithwqueue_last_priv_head[4],
        hvdxg.submithwqueue_last_priv_head[5],
        hvdxg.submithwqueue_last_priv_head[6],
        hvdxg.submithwqueue_last_priv_head[7],
        hvdxg.probe_attempts, hvdxg.probe_successes,
        hvdxg.probe_last_ret, hvdxg.probe_open_status,
        hvdxg.probe_open_handle, hvdxg.probe_open_host_version,
        hvdxg.probe_open_host_compat, hvdxg.probe_info_len,
        hvdxg.probe_info_flags, hvdxg.probe_async_msg_enabled,
        hvdxg.use_ext_header, hvdxg.host_vgpu_luid.b,
        hvdxg.host_vgpu_luid.a,
        hvdxg.pci_dxg_device, hvdxg.pci_dxg_vmbus_version,
        hvdxg.pci_dxg_guid[0], hvdxg.pci_dxg_guid[1],
        hvdxg.pci_dxg_guid[2], hvdxg.pci_dxg_guid[3],
        hvdxg.pci_host_vgpu_luid.b, hvdxg.pci_host_vgpu_luid.a,
        hvdxg.probe_last_type, hvdxg.probe_last_len,
        hvdxg.probe_last_prefix[0], hvdxg.probe_last_prefix[1],
        hvdxg.probe_last_prefix[2], hvdxg.probe_last_prefix[3],
        hvdxg.probe_last_prefix[4], hvdxg.probe_last_prefix[5],
        hvdxg.probe_last_prefix[6], hvdxg.probe_last_prefix[7],
        hvdxg.ioctl_count, hvdxg.ioctl_successes, hvdxg.d3dkmt_ready,
        hvdxg.ioctl_last_ret, hvdxg.dxg_process.v,
        hvdxg.ioctl_history_index,
        HV_DXG_IOCTL_H_ARGS(0), HV_DXG_IOCTL_H_ARGS(1),
        HV_DXG_IOCTL_H_ARGS(2), HV_DXG_IOCTL_H_ARGS(3),
        HV_DXG_IOCTL_H_ARGS(4), HV_DXG_IOCTL_H_ARGS(5),
        HV_DXG_IOCTL_H_ARGS(6), HV_DXG_IOCTL_H_ARGS(7),
        HV_DXG_IOCTL_H_ARGS(8), HV_DXG_IOCTL_H_ARGS(9),
        HV_DXG_IOCTL_H_ARGS(10), HV_DXG_IOCTL_H_ARGS(11),
        HV_DXG_IOCTL_H_ARGS(12), HV_DXG_IOCTL_H_ARGS(13),
        HV_DXG_IOCTL_H_ARGS(14), HV_DXG_IOCTL_H_ARGS(15),
        hvdxg.open_count, hvdxg.live_open_count, hvdxg.cleanup_attempts,
        hvdxg.cleanup_successes, hvdxg.cleanup_last_ret,
        hvdxg.cleanup_last_op, hvdxg.cleanup_last_handle,
        hvdxg.cleanup_failed_op, hvdxg.cleanup_failed_handle,
        hvdxg.cleanup_had_tracked);
    if (len < 0) {
        kvfree(status);
        return -EIO;
    }
    hvdxg_status_append_ioctl_timing(status, status_size, &len,
                                     ioctl_top_nr);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_alloc_priv_in",
                            hvdxg.allocation_last_in_priv_head,
                            hvdxg.allocation_last_in_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_alloc_priv_out",
                            hvdxg.allocation_last_out_priv_head,
                            hvdxg.allocation_last_out_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_context_priv",
                            hvdxg.createcontext_last_priv_head,
                            hvdxg.createcontext_last_priv_head_len);
    hvdxg_status_append_hex(status, status_size, &len, "dxg_hwqueue_priv",
                            hvdxg.createhwqueue_last_priv_head,
                            hvdxg.createhwqueue_last_priv_head_len);
    if ((size_t)len >= status_size) {
        size_t mark = status_size > 96 ? status_size - 96 : 0;

        snprintf(status + mark, status_size - mark,
                 "\ndxg_status_truncated=1 wanted:%d size:%u\n",
                 len, HV_DXG_STATUS_BUF_SIZE);
        len = status_size - 1;
    }
    if (read_status != NULL && read_status_len != NULL) {
        *read_status = status;
        *read_status_len = (size_t)len;
        cached = 1;
    }

copy_cached:
    size_t total = cached && read_status_len != NULL ?
                   *read_status_len : (size_t)len;
    if (*read_offset >= total) {
        *read_emitted = 1;
        if (!cached)
            kvfree(status);
        return 0;
    }
    size_t out = min(count, total - *read_offset);
    if (either_copyout(user, (uint64)buf, status + *read_offset, out) < 0) {
        if (!cached)
            kvfree(status);
        return -EFAULT;
    }
    *read_offset += out;
    if (*read_offset >= total)
        *read_emitted = 1;
    if (!cached)
        kvfree(status);
    return (int)out;
}

static int hvdxg_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    return hvdxg_read_status(cdev, user, buf, count, &hvdxg.read_offset,
                             &hvdxg.read_emitted, NULL, NULL);
}

static void hvdxg_command_vm_init(struct hvdxg_command_vm_to_host *hdr,
                                  uint32 command_type);
static int hvdxg_send_sync_global(const void *cmd, uint32 cmd_len,
                                  void *result, uint32 result_len,
                                  uint32 *actual_len_out);

static uint64 hvdxg_align_up(uint64 value, uint64 align)
{
    return (value + align - 1) & ~(align - 1);
}

static int hvdxg_range_overlaps(uint64 base, uint64 size,
                                uint64 other_base, uint64 other_size,
                                uint64 *other_end_out)
{
    uint64 end = base + size;
    uint64 other_end = other_base + other_size;

    if (other_end_out != NULL)
        *other_end_out = other_end;
    if (size == 0 || other_size == 0 || end <= base ||
        other_end <= other_base)
        return 0;
    return base < other_end && other_base < end;
}

static uint64 hvdxg_pick_iospace_base(uint64 size)
{
    uint64 base = HV_DXG_IOSPACE_SEARCH_START;

    if (size == 0 || size > HV_DXG_IOSPACE_SEARCH_END - base)
        return 0;
    base = hvdxg_align_up(base, HV_DXG_IOSPACE_ALIGN);
    while (base + size > base && base + size <= HV_DXG_IOSPACE_SEARCH_END) {
        uint64 next = 0;

        for (int i = 0; i < platform.mem_count; i++) {
            uint64 other_end = 0;
            if (hvdxg_range_overlaps(base, size, platform.mem[i].base,
                                     platform.mem[i].size, &other_end) &&
                other_end > next)
                next = other_end;
        }
        for (int i = 0; i < platform.reserved_count; i++) {
            uint64 other_end = 0;
            if (hvdxg_range_overlaps(base, size, platform.reserved[i].base,
                                     platform.reserved[i].size, &other_end) &&
                other_end > next)
                next = other_end;
        }
        if (platform.has_framebuffer) {
            uint64 other_end = 0;
            if (hvdxg_range_overlaps(base, size, platform.framebuffer_base,
                                     platform.framebuffer_size,
                                     &other_end) && other_end > next)
                next = other_end;
        }
        if (next == 0)
            return base;
        base = hvdxg_align_up(next, HV_DXG_IOSPACE_ALIGN);
    }
    return 0;
}

static int hvdxg_set_iospace_region(void)
{
    struct hvdxg_command_setiospaceregion cmd;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    uint64 size;
    int ret;

    if (hvdxg.iospace_set)
        return 0;
    if (hvdxg.global_mmio_megabytes == 0) {
        hvdxg.iospace_last_ret = -ENOMEM;
        return -ENOMEM;
    }
    size = (uint64)hvdxg.global_mmio_megabytes << 20;
    size = hvdxg_align_up(size, HV_DXG_IOSPACE_ALIGN);
    hvdxg.iospace_base = hvdxg_pick_iospace_base(size);
    hvdxg.iospace_size = size;
    if (hvdxg.iospace_base == 0) {
        hvdxg.iospace_last_ret = -ENOMEM;
        return -ENOMEM;
    }

    memset(&cmd, 0, sizeof(cmd));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vm_init(&cmd.hdr,
                          HV_DXGK_VMBCOMMAND_SETIOSPACEREGION);
    cmd.start = hvdxg.iospace_base;
    cmd.length = hvdxg.iospace_size;
    ret = hvdxg_send_sync_global(&cmd, sizeof(cmd), &status,
                                 sizeof(status), &actual_len);
    hvdxg.iospace_last_len = actual_len;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.iospace_last_ret = ret;
    if (ret == 0)
        hvdxg.iospace_set = 1;
    return ret;
}

static int hvdxg_iospace_contains(uint64 pa, uint64 size)
{
    uint64 end;

    if (!hvdxg.iospace_set || size == 0)
        return 0;
    end = pa + size;
    if (end < pa)
        return 0;
    return pa >= hvdxg.iospace_base &&
           end <= hvdxg.iospace_base + hvdxg.iospace_size;
}

#ifdef PTE_PWT
#define HV_DXG_PTE_WRITE_THROUGH PTE_PWT
#else
#define HV_DXG_PTE_WRITE_THROUGH 0
#endif

#ifdef PTE_PCD
#define HV_DXG_PTE_UNCACHED (PTE_PCD | HV_DXG_PTE_WRITE_THROUGH)
#else
#define HV_DXG_PTE_UNCACHED HV_DXG_PTE_WRITE_THROUGH
#endif

static uint64 hvdxg_map_iospace_user(uint64 pa, uint64 size,
                                     uint64 extra_pte_flags)
{
    vm_t *vm;
    vma_t *vma;
    uint64 page_pa;
    uint64 page_off;
    uint64 map_size;
    uint64 addr;
    uint64 flags;
    uint64 pte_flags;

    if (current == NULL || current->vm == NULL)
        return 0;
    page_pa = PGROUNDDOWN(pa);
    page_off = pa - page_pa;
    map_size = PGROUNDUP(page_off + size);
    if (map_size == 0 || !hvdxg_iospace_contains(page_pa, map_size))
        return 0;

    vm = current->vm;
    flags = PROT_READ | PROT_WRITE | VMA_FLAG_USER |
            VMA_FLAG_DONTFORK | VMA_FLAG_DONTDUMP | VMA_FLAG_PFNMAP;

    vm_wlock(vm);
    addr = vm_find_free_range(vm, (size_t)map_size, 0);
    if (addr == 0) {
        vm_wunlock(vm);
        return 0;
    }
    vma = vma_alloc(vm, addr, map_size, flags);
    if (vma == NULL) {
        vm_wunlock(vm);
        return 0;
    }
    pte_flags = vma2pte_flags(flags);
    pte_flags |= extra_pte_flags;
    for (uint64 off = 0; off < map_size; off += PGSIZE) {
        if (mappages(vm->pagetable, addr + off, PGSIZE,
                     page_pa + off, pte_flags) != 0) {
            vma_free(vm, vma);
            vm_wunlock(vm);
            return 0;
        }
    }
    vm_wunlock(vm);
    return addr + page_off;
}

static uint64 hvdxg_map_iospace_kernel(uint64 pa, uint64 size)
{
    uint64 page_pa = PGROUNDDOWN(pa);
    uint64 page_off = pa - page_pa;
    uint64 map_size = PGROUNDUP(page_off + size);
    uint64 map_base;
    vma_t *vma;

    if (map_size == 0 || !hvdxg_iospace_contains(page_pa, map_size) ||
        kernel_vm == NULL)
        return 0;

    vm_wlock(kernel_vm);
    map_base = vm_find_free_range(kernel_vm, (size_t)map_size, 0);
    if (map_base == 0) {
        vm_wunlock(kernel_vm);
        return 0;
    }
    vma = vma_alloc(kernel_vm, map_base, map_size,
                    PROT_READ | PROT_WRITE | VMA_FLAG_KERNEL);
    vm_wunlock(kernel_vm);
    if (vma == NULL)
        return 0;

    for (uint64 off = 0; off < map_size; off += PGSIZE) {
        if (arch_vm_map(kernel_pagetable, map_base + off, PGSIZE,
                        page_pa + off, PTE_R | PTE_W) != 0)
            return 0;
    }
    arch_tlb_flush();
    return map_base + page_off;
}

static void hvdxg_unpin_existing_sysmem_pages(uint64 *pages,
                                              uint32 page_count)
{
    if (pages == NULL)
        return;
    for (uint32 i = 0; i < page_count; i++) {
        if (pages[i] != 0)
            (void)page_ref_dec((void *)pages[i]);
    }
    kvfree(pages);
}

static void hvdxg_unpin_tracked_allocation(
    struct hvdxg_tracked_allocation *a)
{
    if (a == NULL || a->sysmem_pages == NULL)
        return;
    hvdxg_unpin_existing_sysmem_pages(a->sysmem_pages,
                                      a->sysmem_page_count);
    a->sysmem_pages = NULL;
    a->sysmem_page_count = 0;
    a->sysmem = 0;
}

static int hvdxg_pin_user_page(vm_t *vm, uint64 uva, int writable,
                               uint64 *out_pa)
{
    uint8 touch = 0;
    uint64 va = PGROUNDDOWN(uva);
    uint64 pa;
    vma_t *vma;
    int ret = 0;

    if (vm == NULL || out_pa == NULL || va >= UVMTOP)
        return -EFAULT;
    if (vm_copyin(vm, &touch, va, sizeof(touch)) < 0)
        return -EFAULT;
    if (writable && vm_copyout(vm, va, &touch, sizeof(touch)) < 0)
        return -EFAULT;

    vm_rlock(vm);
    vma = vm_find_area(vm, va);
    if (vma == NULL ||
        vma_validate(vma, va, PGSIZE,
                     VMA_FLAG_USER | PROT_READ |
                     (writable ? PROT_WRITE : 0)) != 0) {
        ret = -EFAULT;
        goto out;
    }
    pa = walkaddr(vm->pagetable, va);
    if (pa == 0) {
        ret = -EFAULT;
        goto out;
    }
    pa = PGROUNDDOWN(pa);
    if (page_ref_inc((void *)pa) <= 0) {
        ret = -EFAULT;
        goto out;
    }
    *out_pa = pa;
out:
    vm_runlock(vm);
    return ret;
}

static int hvdxg_pin_existing_sysmem(uint64 sysmem, uint64 alloc_size,
                                     int writable, uint64 **pages_out,
                                     uint32 *page_count_out)
{
    uint64 page_count64;
    uint64 *pages = NULL;
    uint32 page_count;

    if (pages_out == NULL || page_count_out == NULL || current == NULL ||
        current->vm == NULL || sysmem == 0 || alloc_size == 0 ||
        (sysmem & (PGSIZE - 1)) != 0 ||
        (alloc_size & (PGSIZE - 1)) != 0)
        return -EINVAL;
    *pages_out = NULL;
    *page_count_out = 0;
    page_count64 = alloc_size >> PGSHIFT;
    if (page_count64 == 0 || page_count64 > 0xffffffffULL)
        return -EINVAL;
    page_count = (uint32)page_count64;
    pages = kvmalloc((size_t)page_count * sizeof(pages[0]));
    if (pages == NULL)
        return -ENOMEM;
    memset(pages, 0, (size_t)page_count * sizeof(pages[0]));
    for (uint32 i = 0; i < page_count; i++) {
        int ret = hvdxg_pin_user_page(current->vm,
                                      sysmem + (uint64)i * PGSIZE,
                                      writable, &pages[i]);

        if (ret != 0) {
            hvdxg_unpin_existing_sysmem_pages(pages, i);
            return ret;
        }
    }
    *pages_out = pages;
    *page_count_out = page_count;
    return 0;
}

static int hvdxg_set_existing_sysmem_pages(uint32 device, uint32 allocation,
                                           const uint64 *pages,
                                           uint32 page_count)
{
    uint8 command_buf[sizeof(struct hvdxg_command_setexistingsysmempages) +
                      (HV_DXG_SYSMEM_PFNS_PER_PACKET - 1) *
                          sizeof(uint64)];
    struct hvdxg_command_setexistingsysmempages *set =
        (struct hvdxg_command_setexistingsysmempages *)command_buf;
    struct hvdxg_ntstatus status;
    uint64 offset = 0;
    uint32 actual_len = 0;
    int ret = 0;

    if (device == 0 || allocation == 0 || pages == NULL || page_count == 0)
        return -EINVAL;

    while (offset < page_count) {
        uint32 n = page_count - (uint32)offset;
        uint32 command_len;

        if (n > HV_DXG_SYSMEM_PFNS_PER_PACKET)
            n = HV_DXG_SYSMEM_PFNS_PER_PACKET;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &set->hdr, HV_DXGK_VMBCOMMAND_SETEXISTINGSYSMEMPAGES,
            hvdxg.dxg_process);
        set->device.v = device;
        set->allocation.v = allocation;
        set->num_pages = n;
        set->alloc_offset_in_pages = (uint32)offset;
        for (uint32 i = 0; i < n; i++)
            set->pfn[i] = pages[offset + i] >> PGSHIFT;
        command_len = sizeof(*set) + (n - 1) * sizeof(uint64);
        ret = hvdxg_send_sync_vgpu(set, command_len, &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        if (ret != 0)
            return ret;
        offset += n;
    }
    return 0;
}

static uint32 hvdxg_device_for_allocation(uint32 allocation)
{
    if (allocation != 0 && allocation == hvdxg.last_allocation_handle)
        return hvdxg.last_allocation_device;
    return hvdxg.last_device_handle;
}

static void hvdxg_untrack_u32(uint32 *items, uint32 *count, uint32 value)
{
    if (value == 0 || count == NULL || items == NULL)
        return;
    for (uint32 i = 0; i < *count; i++) {
        if (items[i] == value) {
            items[i] = items[*count - 1];
            items[*count - 1] = 0;
            (*count)--;
            return;
        }
    }
}

static int hvdxg_has_u32(const uint32 *items, uint32 count, uint32 value)
{
    if (value == 0 || items == NULL)
        return 0;
    for (uint32 i = 0; i < count; i++) {
        if (items[i] == value)
            return 1;
    }
    return 0;
}

static int hvdxg_grow_table(void **items, uint32 *capacity, uint32 need,
                            size_t item_size, uint32 initial_capacity)
{
    void *new_items;
    uint32 new_capacity;
    size_t old_size;
    size_t new_size;

    if (items == NULL || capacity == NULL || item_size == 0)
        return -EINVAL;
    if (*capacity >= need)
        return 0;

    new_capacity = *capacity != 0 ? *capacity : initial_capacity;
    if (new_capacity == 0)
        new_capacity = 16;
    while (new_capacity < need) {
        if (new_capacity > 0xffffffffU / 2)
            return -EOVERFLOW;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > ((size_t)-1) / item_size)
        return -EOVERFLOW;
    old_size = (size_t)(*capacity) * item_size;
    new_size = (size_t)new_capacity * item_size;
    new_items = kvmalloc(new_size);
    if (new_items == NULL)
        return -ENOMEM;
    memset(new_items, 0, new_size);
    if (*items != NULL && old_size != 0) {
        memcpy(new_items, *items, old_size);
        kvfree(*items);
    }
    *items = new_items;
    *capacity = new_capacity;
    return 0;
}

static int hvdxg_track_u32_grow(uint32 **items, uint32 *count,
                                uint32 *capacity, uint32 value)
{
    int ret;

    if (value == 0 || items == NULL || count == NULL || capacity == NULL)
        return 0;
    for (uint32 i = 0; i < *count; i++) {
        if ((*items)[i] == value)
            return 0;
    }
    ret = hvdxg_grow_table((void **)items, capacity, *count + 1,
                           sizeof((*items)[0]), HV_DXG_OPEN_TRACKED_MAX);
    if (ret != 0)
        return ret;
    (*items)[(*count)++] = value;
    return 0;
}

static void hvdxg_track_sync(struct hvdxg_open_state *owner,
                             uint32 device, uint32 sync, uint32 type,
                             uint32 flags, uint32 global_shared,
                             uint64 fence_cpu_va, uint64 fence_kva)
{
    if (owner == NULL || sync == 0)
        return;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync) {
            owner->sync_objects[i].type = type;
            owner->sync_objects[i].device = device;
            owner->sync_objects[i].flags = flags;
            owner->sync_objects[i].global_shared = global_shared;
            owner->sync_objects[i].fence_cpu_va = fence_cpu_va;
            owner->sync_objects[i].fence_kva = fence_kva;
            return;
        }
    }
    if (hvdxg_grow_table((void **)&owner->sync_objects,
                         &owner->sync_object_capacity,
                         owner->sync_object_count + 1,
                         sizeof(owner->sync_objects[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        uint32 i = owner->sync_object_count++;

        owner->sync_objects[i].sync = sync;
        owner->sync_objects[i].type = type;
        owner->sync_objects[i].device = device;
        owner->sync_objects[i].flags = flags;
        owner->sync_objects[i].global_shared = global_shared;
        owner->sync_objects[i].fence_cpu_va = fence_cpu_va;
        owner->sync_objects[i].fence_kva = fence_kva;
    }
}

static void hvdxg_untrack_sync(struct hvdxg_open_state *owner, uint32 sync)
{
    if (owner == NULL || sync == 0)
        return;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync) {
            uint32 last = owner->sync_object_count - 1;

            owner->sync_objects[i] = owner->sync_objects[last];
            memset(&owner->sync_objects[last], 0,
                   sizeof(owner->sync_objects[0]));
            owner->sync_object_count--;
            return;
        }
    }
}

static uint32 hvdxg_owner_sync_type(struct hvdxg_open_state *owner,
                                    uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].type;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_device(struct hvdxg_open_state *owner,
                                      uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].device;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_flags(struct hvdxg_open_state *owner,
                                     uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].flags;
    }
    return 0;
}

static uint32 hvdxg_owner_sync_global_shared(struct hvdxg_open_state *owner,
                                             uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].global_shared;
    }
    return 0;
}

static uint64 hvdxg_owner_sync_fence_kva(struct hvdxg_open_state *owner,
                                         uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return owner->sync_objects[i].fence_kva;
    }
    return 0;
}

static int hvdxg_sync_cpu_fence_satisfied(uint64 fence_kva,
                                          uint64 fence_value)
{
    volatile uint64 *current_value;

    if (fence_kva == 0)
        return 0;
    current_value = (volatile uint64 *)fence_kva;
    return *current_value >= fence_value;
}

static int hvdxg_wait_cpu_fences_already_satisfied(
    struct hvdxg_open_state *owner,
    const struct hvdxg_d3dkmthandle *objects,
    const uint64 *fence_values, uint32 object_count, int wait_any)
{
    int satisfied = wait_any ? 0 : 1;

    for (uint32 i = 0; i < object_count; i++) {
        uint64 fence_kva =
            hvdxg_owner_sync_fence_kva(owner, objects[i].v);
        int one_satisfied =
            hvdxg_sync_cpu_fence_satisfied(fence_kva, fence_values[i]);

        if (wait_any && one_satisfied)
            return 1;
        if (!wait_any && !one_satisfied)
            satisfied = 0;
    }
    return satisfied;
}

static int hvdxg_owner_sync_is_monitored(struct hvdxg_open_state *owner,
                                         uint32 sync)
{
    uint32 type = hvdxg_owner_sync_type(owner, sync);

    return type == _D3DDDI_MONITORED_FENCE ||
           type == _D3DDDI_PERIODIC_MONITORED_FENCE;
}

static void hvdxg_track_hwqueue(struct hvdxg_open_state *owner,
                                uint32 queue, uint32 sync_object)
{
    if (owner == NULL || queue == 0)
        return;
    for (uint32 i = 0; i < owner->hwqueue_count; i++) {
        if (owner->hwqueues[i].queue == queue) {
            owner->hwqueues[i].sync_object = sync_object;
            return;
        }
    }
    if (hvdxg_grow_table((void **)&owner->hwqueues,
                         &owner->hwqueue_capacity,
                         owner->hwqueue_count + 1,
                         sizeof(owner->hwqueues[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        uint32 i = owner->hwqueue_count++;
        owner->hwqueues[i].queue = queue;
        owner->hwqueues[i].sync_object = sync_object;
        if (owner->hwqueue_count > hvdxg.track_hwqueue_max)
            hvdxg.track_hwqueue_max = owner->hwqueue_count;
    } else {
        hvdxg.track_hwqueue_drops++;
    }
}

static uint32 hvdxg_untrack_hwqueue(struct hvdxg_open_state *owner,
                                    uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    for (uint32 i = 0; i < owner->hwqueue_count; i++) {
        if (owner->hwqueues[i].queue == queue) {
            uint32 sync = owner->hwqueues[i].sync_object;
            uint32 last = owner->hwqueue_count - 1;

            owner->hwqueues[i] = owner->hwqueues[last];
            memset(&owner->hwqueues[last], 0,
                   sizeof(owner->hwqueues[0]));
            owner->hwqueue_count--;
            return sync;
        }
    }
    return 0;
}

static void hvdxg_track_pagingqueue(struct hvdxg_open_state *owner,
                                    uint32 device, uint32 queue,
                                    uint32 sync_object, uint64 fence_pa)
{
    if (owner == NULL || queue == 0)
        return;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue) {
            owner->paging_queues[i].device = device;
            owner->paging_queues[i].sync_object = sync_object;
            owner->paging_queues[i].fence_pa = fence_pa;
            return;
        }
    }
    if (hvdxg_grow_table((void **)&owner->paging_queues,
                         &owner->paging_queue_capacity,
                         owner->paging_queue_count + 1,
                         sizeof(owner->paging_queues[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        uint32 i = owner->paging_queue_count++;
        owner->paging_queues[i].queue = queue;
        owner->paging_queues[i].device = device;
        owner->paging_queues[i].sync_object = sync_object;
        owner->paging_queues[i].fence_pa = fence_pa;
        if (owner->paging_queue_count > hvdxg.track_pagingqueue_max)
            hvdxg.track_pagingqueue_max = owner->paging_queue_count;
    } else {
        hvdxg.track_pagingqueue_drops++;
    }
}

static uint32 hvdxg_untrack_pagingqueue(struct hvdxg_open_state *owner,
                                        uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue) {
            uint32 sync = owner->paging_queues[i].sync_object;
            uint32 last = owner->paging_queue_count - 1;

            owner->paging_queues[i] = owner->paging_queues[last];
            memset(&owner->paging_queues[last], 0,
                   sizeof(owner->paging_queues[0]));
            owner->paging_queue_count--;
            return sync;
        }
    }
    return 0;
}

static uint64 hvdxg_owner_pagingqueue_fence_pa(struct hvdxg_open_state *owner,
                                               uint32 queue)
{
    if (owner == NULL || queue == 0)
        return 0;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == queue)
            return owner->paging_queues[i].fence_pa;
    }
    return 0;
}

static int hvdxg_owner_has_device(struct hvdxg_open_state *owner,
                                  uint32 device)
{
    return owner != NULL &&
           hvdxg_has_u32(owner->devices, owner->device_count, device);
}

static int hvdxg_owner_has_pagingqueue(struct hvdxg_open_state *owner,
                                       uint32 paging_queue)
{
    if (owner == NULL || paging_queue == 0)
        return 0;
    for (uint32 i = 0; i < owner->paging_queue_count; i++) {
        if (owner->paging_queues[i].queue == paging_queue)
            return 1;
    }
    return 0;
}

static int hvdxg_owner_has_sync(struct hvdxg_open_state *owner, uint32 sync)
{
    if (owner == NULL || sync == 0)
        return 0;
    for (uint32 i = 0; i < owner->sync_object_count; i++) {
        if (owner->sync_objects[i].sync == sync)
            return 1;
    }
    return 0;
}

static int hvdxg_owner_has_context(struct hvdxg_open_state *owner,
                                   uint32 context)
{
    return owner != NULL &&
           hvdxg_has_u32(owner->contexts, owner->context_count, context);
}

static int hvdxg_owner_has_hwqueue(struct hvdxg_open_state *owner,
                                   uint32 hwqueue)
{
    if (owner == NULL || hwqueue == 0)
        return 0;
    for (uint32 i = 0; i < owner->hwqueue_count; i++) {
        if (owner->hwqueues[i].queue == hwqueue)
            return 1;
    }
    return 0;
}

static void hvdxg_track_allocation(struct hvdxg_open_state *owner,
                                   uint32 device, uint32 resource,
                                   uint32 allocation, uint64 size,
                                   uint32 flags, uint64 sysmem,
                                   uint64 *sysmem_pages,
                                   uint32 sysmem_page_count)
{
    if (owner == NULL || device == 0 ||
        (resource == 0 && allocation == 0))
        return;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if (owner->allocations[i].device == device &&
            owner->allocations[i].resource == resource &&
            owner->allocations[i].allocation == allocation) {
            if (owner->allocations[i].sysmem_pages != sysmem_pages)
                hvdxg_unpin_tracked_allocation(&owner->allocations[i]);
            owner->allocations[i].size = size;
            owner->allocations[i].flags = flags;
            owner->allocations[i].sysmem = sysmem;
            owner->allocations[i].sysmem_pages = sysmem_pages;
            owner->allocations[i].sysmem_page_count = sysmem_page_count;
            return;
        }
    }
    if (hvdxg_grow_table((void **)&owner->allocations,
                         &owner->allocation_capacity,
                         owner->allocation_count + 1,
                         sizeof(owner->allocations[0]),
                         HV_DXG_OPEN_TRACKED_MAX) == 0) {
        struct hvdxg_tracked_allocation *slot =
            &owner->allocations[owner->allocation_count++];

        memset(slot, 0, sizeof(*slot));
        slot->device = device;
        slot->resource = resource;
        slot->allocation = allocation;
        slot->size = size;
        slot->flags = flags;
        slot->sysmem = sysmem;
        slot->sysmem_pages = sysmem_pages;
        slot->sysmem_page_count = sysmem_page_count;
        if (owner->allocation_count > hvdxg.track_allocation_max)
            hvdxg.track_allocation_max = owner->allocation_count;
    } else {
        hvdxg_unpin_existing_sysmem_pages(sysmem_pages, sysmem_page_count);
        hvdxg.track_allocation_drops++;
    }
}

static int hv_cmdline_enabled(const char *key);

static struct hvdxg_tracked_allocation *
hvdxg_owner_find_allocation(struct hvdxg_open_state *owner, uint32 device,
                            uint32 resource, uint32 allocation)
{
    if (owner == NULL || (resource == 0 && allocation == 0))
        return NULL;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            (resource == 0 || owner->allocations[i].resource == resource) &&
            (allocation == 0 ||
             owner->allocations[i].allocation == allocation))
            return &owner->allocations[i];
    }
    return NULL;
}

static int hvdxg_owner_has_allocation(struct hvdxg_open_state *owner,
                                      uint32 device, uint32 resource,
                                      uint32 allocation)
{
    if (owner == NULL || (resource == 0 && allocation == 0))
        return 0;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            (resource == 0 || owner->allocations[i].resource == resource) &&
            (allocation == 0 ||
             owner->allocations[i].allocation == allocation))
            return 1;
    }
    return 0;
}

static uint64 hvdxg_owner_allocation_size(struct hvdxg_open_state *owner,
                                          uint32 device, uint32 resource,
                                          uint32 allocation)
{
    struct hvdxg_tracked_allocation *a =
        hvdxg_owner_find_allocation(owner, device, resource, allocation);

    return a != NULL ? a->size : 0;
}

static int hvdxg_unmap_tracked_allocation(
    struct hvdxg_tracked_allocation *a)
{
    uint64 map_va;

    if (a == NULL || a->cpu_va == 0 || a->map_size == 0)
        return 0;
    if (current == NULL || current->vm == NULL)
        return 0;
    if (a->cpu_vm != NULL && a->cpu_vm != current->vm)
        return 0;
    map_va = PGROUNDDOWN(a->cpu_va);
    if (vm_munmap_region(current->vm, map_va, (size_t)a->map_size) != 0)
        return -EINVAL;
    a->cpu_va = 0;
    a->map_size = 0;
    a->lock_refcount = 0;
    a->cpu_vm = NULL;
    return 0;
}

static void hvdxg_untrack_allocation(struct hvdxg_open_state *owner,
                                     uint32 device, uint32 resource,
                                     uint32 allocation)
{
    if (owner == NULL)
        return;
    for (uint32 i = 0; i < owner->allocation_count; i++) {
        if ((device == 0 || owner->allocations[i].device == device) &&
            (resource == 0 || owner->allocations[i].resource == resource) &&
            (allocation == 0 ||
             owner->allocations[i].allocation == allocation)) {
            (void)hvdxg_unmap_tracked_allocation(&owner->allocations[i]);
            hvdxg_unpin_tracked_allocation(&owner->allocations[i]);
            owner->allocations[i] =
                owner->allocations[owner->allocation_count - 1];
            memset(&owner->allocations[owner->allocation_count - 1], 0,
                   sizeof(owner->allocations[0]));
            owner->allocation_count--;
            i--;
        }
    }
}

static void hvdxg_free_tracked_resource(struct hvdxg_tracked_resource *r)
{
    if (r == NULL)
        return;
    if (r->private_runtime_data != NULL)
        kvfree(r->private_runtime_data);
    if (r->resource_priv_drv_data != NULL)
        kvfree(r->resource_priv_drv_data);
    if (r->total_priv_drv_data != NULL)
        kvfree(r->total_priv_drv_data);
    memset(r, 0, sizeof(*r));
}

static void hvdxg_untrack_resource(struct hvdxg_open_state *owner,
                                   uint32 device, uint32 resource)
{
    if (owner == NULL || resource == 0)
        return;
    for (uint32 i = 0; i < owner->resource_count; i++) {
        if ((device == 0 || owner->resources[i].device == device) &&
            owner->resources[i].resource == resource) {
            uint32 last = owner->resource_count - 1;

            hvdxg_free_tracked_resource(&owner->resources[i]);
            if (i != last)
                owner->resources[i] = owner->resources[last];
            memset(&owner->resources[last], 0, sizeof(owner->resources[0]));
            owner->resource_count--;
            return;
        }
    }
}

static struct hvdxg_tracked_resource *
hvdxg_owner_find_resource(struct hvdxg_open_state *owner, uint32 device,
                          uint32 resource)
{
    if (owner == NULL || resource == 0)
        return NULL;
    for (uint32 i = 0; i < owner->resource_count; i++) {
        if ((device == 0 || owner->resources[i].device == device) &&
            owner->resources[i].resource == resource)
            return &owner->resources[i];
    }
    return NULL;
}

static int hvdxg_copy_user_bytes(uint8 **dst, uint64 src, uint32 size)
{
    *dst = NULL;
    if (size == 0)
        return 0;
    if (src == 0)
        return -EINVAL;
    *dst = kvmalloc(size);
    if (*dst == NULL)
        return -ENOMEM;
    if (either_copyin(*dst, 1, src, size) < 0) {
        kvfree(*dst);
        *dst = NULL;
        return -EFAULT;
    }
    return 0;
}

static int hvdxg_copy_kernel_bytes(uint8 **dst, const uint8 *src, uint32 size)
{
    *dst = NULL;
    if (size == 0)
        return 0;
    if (src == NULL)
        return -EINVAL;
    *dst = kvmalloc(size);
    if (*dst == NULL)
        return -ENOMEM;
    memmove(*dst, src, size);
    return 0;
}

static int hvdxg_get_standard_alloc_priv_data(
    uint32 device, const struct d3dkmt_createstandardallocation *standard_alloc,
    uint32 *alloc_priv_size, uint8 **alloc_priv_data,
    uint32 *res_priv_size, uint8 **res_priv_data)
{
    struct hvdxg_command_getstandardallocprivdata query;
    struct hvdxg_command_getstandardallocprivdata_return *result = NULL;
    uint32 result_len = sizeof(*result);
    uint32 actual_len = 0;
    uint32 alloc_size = 0;
    uint32 res_size = 0;
    int ret;

    if (alloc_priv_size != NULL)
        *alloc_priv_size = 0;
    if (alloc_priv_data != NULL)
        *alloc_priv_data = NULL;
    if (res_priv_size != NULL)
        *res_priv_size = 0;
    if (res_priv_data != NULL)
        *res_priv_data = NULL;
    if (device == 0 || standard_alloc == NULL || alloc_priv_size == NULL ||
        alloc_priv_data == NULL || res_priv_size == NULL ||
        res_priv_data == NULL)
        return -EINVAL;

    memset(&query, 0, sizeof(query));
    hvdxg_command_vgpu_init_process(
        &query.hdr, HV_DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA,
        hvdxg.dxg_process);
    query.alloc_type = _D3DKMDT_STANDARDALLOCATION_GDISURFACE;
    query.gdi_surface.type = _D3DKMDT_GDISURFACE_TEXTURE_CROSSADAPTER;
    query.gdi_surface.width =
        (uint32)standard_alloc->existing_heap_data.size;
    query.gdi_surface.height = 1;
    query.gdi_surface.format = _D3DDDIFMT_UNKNOWN;

    hvdxg.stdalloc_last_len = 0;
    hvdxg.stdalloc_last_ret = 0;
    hvdxg.stdalloc_last_status = 0;
    hvdxg.stdalloc_last_alloc_size = 0;
    hvdxg.stdalloc_last_res_size = 0;
    result = kvmalloc(result_len);
    if (result == NULL)
        return -ENOMEM;
    memset(result, 0, result_len);

    ret = hvdxg_send_sync_vgpu(&query, sizeof(query), result, result_len,
                               &actual_len);
    hvdxg.stdalloc_last_len = actual_len;
    hvdxg.stdalloc_last_ret = ret;
    if (ret != 0)
        goto done;
    if (actual_len < sizeof(*result)) {
        ret = -EOVERFLOW;
        goto done;
    }
    hvdxg.stdalloc_last_status = result->status.v;
    ret = hvdxg_ntstatus_to_errno(result->status);
    if (ret != 0)
        goto done;
    alloc_size = result->priv_driver_data_size;
    res_size = result->priv_driver_resource_size;
    hvdxg.stdalloc_last_alloc_size = alloc_size;
    hvdxg.stdalloc_last_res_size = res_size;
    if (alloc_size == 0 ||
        alloc_size > HV_DXG_VM_BUS_PACKET_MAX ||
        res_size > HV_DXG_VM_BUS_PACKET_MAX) {
        ret = -EINVAL;
        goto done;
    }
    kvfree(result);
    result_len = sizeof(*result) + alloc_size + res_size;
    if (result_len > HV_DXG_VM_BUS_PACKET_MAX) {
        ret = -EOVERFLOW;
        result = NULL;
        goto done;
    }
    result = kvmalloc(result_len);
    if (result == NULL) {
        ret = -ENOMEM;
        goto done;
    }
    memset(result, 0, result_len);
    query.priv_driver_data_size = alloc_size;
    query.priv_driver_resource_size = res_size;
    actual_len = 0;
    ret = hvdxg_send_sync_vgpu(&query, sizeof(query), result, result_len,
                               &actual_len);
    hvdxg.stdalloc_last_len = actual_len;
    hvdxg.stdalloc_last_ret = ret;
    if (ret != 0)
        goto done;
    if (actual_len < sizeof(*result) + alloc_size + res_size) {
        ret = -EOVERFLOW;
        goto done;
    }
    hvdxg.stdalloc_last_status = result->status.v;
    ret = hvdxg_ntstatus_to_errno(result->status);
    if (ret != 0)
        goto done;
    if (result->priv_driver_data_size != alloc_size ||
        result->priv_driver_resource_size != res_size) {
        ret = -EOVERFLOW;
        goto done;
    }
    ret = hvdxg_copy_kernel_bytes(alloc_priv_data,
                                  (const uint8 *)&result[1],
                                  alloc_size);
    if (ret != 0)
        goto done;
    ret = hvdxg_copy_kernel_bytes(res_priv_data,
                                  (const uint8 *)&result[1] + alloc_size,
                                  res_size);
    if (ret != 0) {
        if (*alloc_priv_data != NULL) {
            kvfree(*alloc_priv_data);
            *alloc_priv_data = NULL;
        }
        goto done;
    }
    *alloc_priv_size = alloc_size;
    *res_priv_size = res_size;

done:
    hvdxg.stdalloc_last_ret = ret;
    if (result != NULL)
        kvfree(result);
    return ret;
}

static void hvdxg_track_resource(struct hvdxg_open_state *owner,
                                 struct d3dkmt_createallocation *req,
                                 struct d3dkmt_createallocationflags requested_flags,
                                 struct d3dddi_allocationinfo2 *alloc_info,
                                 struct hvdxg_command_createallocation_return *result,
                                 const uint8 *alloc_private_data,
                                 uint32 total_alloc_private,
                                 const uint8 *resource_priv_data,
                                 uint32 resource_priv_data_size)
{
    struct hvdxg_tracked_resource tmp;
    struct hvdxg_tracked_resource *slot;
    int ret;

    if (owner == NULL || req == NULL || result == NULL ||
        req->resource.v == 0 || !req->flags.create_resource)
        return;
    if (req->alloc_count == 0 || req->alloc_count > HV_DXG_ALLOCATION_MAX)
        return;

    memset(&tmp, 0, sizeof(tmp));
    tmp.device = req->device.v;
    tmp.resource = req->resource.v;
    tmp.global_share = req->global_share.v != 0 ?
                       req->global_share.v : result->global_share.v;
    tmp.allocation_count = req->alloc_count;
    tmp.create_shared = requested_flags.create_shared;
    tmp.nt_security_sharing = requested_flags.nt_security_sharing;
    tmp.private_runtime_data_size = req->private_runtime_data_size;
    tmp.resource_priv_drv_data_size = resource_priv_data_size;
    tmp.total_priv_drv_data_size = total_alloc_private;
    for (uint32 i = 0; i < req->alloc_count; i++) {
        tmp.alloc_priv_sizes[i] = alloc_info[i].priv_drv_data_size;
        tmp.allocation_sizes[i] =
            result->allocation_info[i].allocation_size;
        tmp.allocation_flags[i] =
            result->allocation_info[i].allocation_flags;
    }
    if (req->flags.standard_allocation && req->alloc_count == 1 &&
        total_alloc_private != 0)
        tmp.alloc_priv_sizes[0] = total_alloc_private;

    ret = hvdxg_copy_user_bytes(&tmp.private_runtime_data,
                                req->private_runtime_data,
                                tmp.private_runtime_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&tmp.resource_priv_drv_data,
                                  resource_priv_data,
                                  tmp.resource_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&tmp.total_priv_drv_data,
                                  alloc_private_data,
                                  tmp.total_priv_drv_data_size);
    if (ret != 0)
        goto fail;

    slot = hvdxg_owner_find_resource(owner, tmp.device, tmp.resource);
    if (slot == NULL) {
        if (hvdxg_grow_table((void **)&owner->resources,
                             &owner->resource_capacity,
                             owner->resource_count + 1,
                             sizeof(owner->resources[0]),
                             HV_DXG_RESOURCE_TRACKED_MAX) != 0)
            goto fail;
        slot = &owner->resources[owner->resource_count++];
    } else {
        hvdxg_free_tracked_resource(slot);
    }
    *slot = tmp;
    return;

fail:
    hvdxg_free_tracked_resource(&tmp);
}

static int hvdxg_clone_resource(struct hvdxg_tracked_resource *dst,
                                const struct hvdxg_tracked_resource *src)
{
    int ret;

    memset(dst, 0, sizeof(*dst));
    *dst = *src;
    dst->private_runtime_data = NULL;
    dst->resource_priv_drv_data = NULL;
    dst->total_priv_drv_data = NULL;
    ret = hvdxg_copy_kernel_bytes(&dst->private_runtime_data,
                                  src->private_runtime_data,
                                  src->private_runtime_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&dst->resource_priv_drv_data,
                                  src->resource_priv_drv_data,
                                  src->resource_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    ret = hvdxg_copy_kernel_bytes(&dst->total_priv_drv_data,
                                  src->total_priv_drv_data,
                                  src->total_priv_drv_data_size);
    if (ret != 0)
        goto fail;
    return 0;

fail:
    hvdxg_free_tracked_resource(dst);
    return ret;
}

static int hvdxg_create_nt_shared_object(uint32 object, uint32 *shared_handle)
{
    struct hvdxg_command_createntsharedobject create;
    struct hvdxg_d3dkmthandle result;
    uint32 actual_len = 0;
    int ret;

    if (shared_handle != NULL)
        *shared_handle = 0;
    if (object == 0 || shared_handle == NULL)
        return -EINVAL;

    memset(&create, 0, sizeof(create));
    memset(&result, 0, sizeof(result));
    hvdxg.ntshared_last_create_len = 0;
    hvdxg.ntshared_last_create_ret = 0;
    hvdxg.ntshared_last_create_object = object;
    hvdxg.ntshared_last_create_handle = 0;
    hvdxg.ntshared_last_create_raw0 = 0;
    hvdxg_command_vm_init(&create.hdr,
                          HV_DXGK_VMBCOMMAND_CREATENTSHAREDOBJECT);
    create.hdr.process = hvdxg.dxg_process;
    create.object.v = object;

    ret = hvdxg_send_sync_global(&create, sizeof(create), &result,
                                 sizeof(result), &actual_len);
    hvdxg.ntshared_last_create_len = actual_len;
    hvdxg.ntshared_last_create_raw0 = result.v;
    if (ret == 0 && actual_len < sizeof(result))
        ret = -EOVERFLOW;
    if (ret == 0 && result.v == 0)
        ret = -EIO;
    if (ret == 0) {
        *shared_handle = result.v;
        hvdxg.ntshared_last_create_handle = result.v;
    }
    hvdxg.ntshared_last_create_ret = ret;
    return ret;
}

static int hvdxg_destroy_nt_shared_object(uint32 shared_handle)
{
    struct hvdxg_command_destroyntsharedobject destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (shared_handle == 0)
        return 0;

    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg.ntshared_last_destroy_len = 0;
    hvdxg.ntshared_last_destroy_ret = 0;
    hvdxg.ntshared_last_destroy_handle = shared_handle;
    hvdxg_command_vm_init(&destroy.hdr,
                          HV_DXGK_VMBCOMMAND_DESTROYNTSHAREDOBJECT);
    destroy.shared_handle.v = shared_handle;

    ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                 sizeof(status), &actual_len);
    hvdxg.ntshared_last_destroy_len = actual_len;
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.ntshared_last_destroy_ret = ret;
    return ret;
}

static int hvdxg_shared_object_release(struct vfs_inode *ip,
                                       struct vfs_file *file)
{
    struct hvdxg_shared_object *shared =
        file != NULL ? (struct hvdxg_shared_object *)file->private_data : NULL;

    (void)ip;
    if (shared != NULL) {
        if (shared->host_nt_handle != 0)
            (void)hvdxg_destroy_nt_shared_object(shared->host_nt_handle);
        if (shared->kind == HV_DXG_SHARED_OBJECT_RESOURCE)
            hvdxg_free_tracked_resource(&shared->resource);
        kvfree(shared);
        file->private_data = NULL;
    }
    return 0;
}

static struct vfs_file_ops hvdxg_shared_object_file_ops = {
    .release = hvdxg_shared_object_release,
};

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
    if (f->ops != &hvdxg_shared_object_file_ops ||
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

static int hvdxg_share_object_with_host(uint32 device, uint32 object,
                                        uint64 reserved, uint64 *nt_handle,
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
    if (device == 0 || object == 0)
        return -EINVAL;
    memset(&share, 0, sizeof(share));
    memset(&result, 0, sizeof(result));
    hvdxg_command_vm_init(&share.hdr,
                          HV_DXGK_VMBCOMMAND_SHAREOBJECTWITHHOST);
    share.hdr.process = hvdxg.dxg_process;
    share.device_handle.v = device;
    share.object_handle.v = object;
    share.reserved = reserved;
    ret = hvdxg_send_sync_global(&share, sizeof(share), &result,
                                 sizeof(result), &actual_len);
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

static int hvdxg_owner_has_gpuva(struct hvdxg_open_state *owner,
                                 uint32 adapter, uint64 base)
{
    if (owner == NULL || base == 0)
        return 0;
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        if ((adapter == 0 || owner->gpuvas[i].adapter == adapter) &&
            owner->gpuvas[i].base == base)
            return 1;
    }
    return 0;
}

static uint64 hvdxg_owner_gpuva_size(struct hvdxg_open_state *owner,
                                     uint32 adapter, uint64 base)
{
    if (owner == NULL || base == 0)
        return 0;
    for (uint32 i = 0; i < owner->gpuva_count; i++) {
        if ((adapter == 0 || owner->gpuvas[i].adapter == adapter) &&
            owner->gpuvas[i].base == base)
            return owner->gpuvas[i].size;
    }
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

static void hvdxg_track_gpuva(struct hvdxg_open_state *owner, uint32 adapter,
                              uint64 base, uint64 size, uint64 fence_value,
                              uint64 fence_cpu_pa)
{
    if (owner == NULL || adapter == 0 || base == 0 || size == 0)
        return;
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

static int hvdxg_destroy_device_host(uint32 device)
{
    struct hvdxg_command_destroydevice destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (device == 0 || device == hvdxg.host_adapter_handle)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYDEVICE,
                                    hvdxg.dxg_process);
    destroy.device.v = device;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    return ret;
}

static int hvdxg_destroy_allocation_host(uint32 device, uint32 resource,
                                         uint32 allocation)
{
    uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                      sizeof(struct hvdxg_d3dkmthandle)];
    struct hvdxg_command_destroyallocation *destroy =
        (struct hvdxg_command_destroyallocation *)command_buf;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    uint32 command_len;
    int ret;

    if (device == 0 || (resource == 0 && allocation == 0))
        return 0;
    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy->hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                    hvdxg.dxg_process);
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
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroyalloc_last_len = actual_len;
    hvdxg.destroyalloc_last_ret = ret;
    return ret;
}

static int hvdxg_destroy_createallocation_result(
    uint32 device, uint32 resource,
    const struct hvdxg_command_createallocation_return *result,
    uint32 alloc_count)
{
    uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                      HV_DXG_ALLOCATION_MAX *
                          sizeof(struct hvdxg_d3dkmthandle)];
    struct hvdxg_command_destroyallocation *destroy =
        (struct hvdxg_command_destroyallocation *)command_buf;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    uint32 command_len;
    int ret;

    if (device == 0 || result == NULL || alloc_count == 0 ||
        alloc_count > HV_DXG_ALLOCATION_MAX)
        return 0;
    memset(command_buf, 0, sizeof(command_buf));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy->hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                    hvdxg.dxg_process);
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
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroyalloc_last_len = actual_len;
    hvdxg.destroyalloc_last_ret = ret;
    hvdxg.createalloc_unwind_attempts++;
    hvdxg.createalloc_unwind_last_ret = ret;
    if (ret == 0)
        hvdxg.createalloc_unwind_successes++;
    return ret;
}

static int hvdxg_destroy_pagingqueue_host(uint32 paging_queue)
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
                                    hvdxg.dxg_process);
    destroy.paging_queue.v = paging_queue;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    return ret;
}

static int hvdxg_destroy_sync_host(uint32 sync_object)
{
    struct hvdxg_command_destroysyncobject destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (sync_object == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vm_init(&destroy.hdr,
                          HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT);
    destroy.hdr.process = hvdxg.dxg_process;
    destroy.sync_object.v = sync_object;
    ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                 sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    return ret;
}

static int hvdxg_destroy_context_host(uint32 context)
{
    struct hvdxg_command_destroycontext destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (context == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYCONTEXT,
                                    hvdxg.dxg_process);
    destroy.context.v = context;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    return ret;
}

static int hvdxg_destroy_hwqueue_host(uint32 hwqueue)
{
    struct hvdxg_command_destroyhwqueue destroy;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (hwqueue == 0)
        return 0;
    memset(&destroy, 0, sizeof(destroy));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&destroy.hdr,
                                    HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE,
                                    hvdxg.dxg_process);
    destroy.hwqueue.v = hwqueue;
    ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.destroyhwqueue_last_len = actual_len;
    hvdxg.destroyhwqueue_last_ret = ret;
    return ret;
}

static int hvdxg_free_gpuva_host(uint32 adapter, uint64 base, uint64 size)
{
    struct hvdxg_command_freegpuvirtualaddress freeva;
    struct hvdxg_ntstatus status;
    uint32 actual_len = 0;
    int ret;

    if (adapter == 0 || base == 0 || size == 0)
        return 0;
    memset(&freeva, 0, sizeof(freeva));
    memset(&status, 0, sizeof(status));
    hvdxg_command_vgpu_init_process(&freeva.hdr,
                                    HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS,
                                    hvdxg.dxg_process);
    freeva.args.adapter.v = adapter;
    freeva.args.base_address = base;
    freeva.args.size = size;
    ret = hvdxg_send_sync_vgpu(&freeva, sizeof(freeva), &status,
                               sizeof(status), &actual_len);
    if (ret == 0 && actual_len >= sizeof(status))
        ret = hvdxg_ntstatus_to_errno(status);
    hvdxg.gpuva_free_last_len = actual_len;
    hvdxg.gpuva_free_last_ret = ret;
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

enum {
    HV_DXG_CLEANUP_NONE = 0,
    HV_DXG_CLEANUP_HWQUEUE = 1,
    HV_DXG_CLEANUP_SYNC = 2,
    HV_DXG_CLEANUP_CONTEXT = 3,
    HV_DXG_CLEANUP_GPUVA = 4,
    HV_DXG_CLEANUP_ALLOCATION = 5,
    HV_DXG_CLEANUP_PAGINGQUEUE = 6,
    HV_DXG_CLEANUP_DEVICE = 7,
};

static void hvdxg_cleanup_note_ret(int *cleanup_ret, int op_ret,
                                   uint32 op, uint32 handle)
{
    hvdxg.cleanup_last_op = op;
    hvdxg.cleanup_last_handle = handle;
    if (*cleanup_ret == 0 && op_ret != 0) {
        *cleanup_ret = op_ret;
        hvdxg.cleanup_failed_op = op;
        hvdxg.cleanup_failed_handle = handle;
    }
}

static int hvdxg_bind_open_process(struct hvdxg_open_state *owner)
{
    int ret;

    if (owner == NULL)
        return 0;
    ret = hvdxg_d3dkmt_ensure();
    if (ret != 0)
        return ret;
    owner->dxg_process = hvdxg.dxg_process;
    owner->dxg_process_created = hvdxg.dxg_process_created;
    return 0;
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
        hvdxg.d3dkmt_ready = 1;
    } else if (!hvdxg.d3dkmt_ready) {
        return;
    }
    had_tracked = owner->device_count != 0 || owner->allocation_count != 0 ||
                  owner->gpuva_count != 0 || owner->sync_object_count != 0 ||
                  owner->paging_queue_count != 0 ||
                  owner->context_count != 0 || owner->hwqueue_count != 0;
    hvdxg.cleanup_last_op = HV_DXG_CLEANUP_NONE;
    hvdxg.cleanup_last_handle = 0;
    hvdxg.cleanup_failed_op = HV_DXG_CLEANUP_NONE;
    hvdxg.cleanup_failed_handle = 0;
    hvdxg.cleanup_had_tracked = had_tracked ? 1 : 0;
    if (owner->hwqueues == NULL)
        owner->hwqueue_count = 0;
    while (owner->hwqueue_count > 0) {
        uint32 handle = owner->hwqueues[owner->hwqueue_count - 1].queue;
        uint32 sync = hvdxg_untrack_hwqueue(owner, handle);

        if (sync != 0)
            hvdxg_untrack_sync(owner, sync);
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_hwqueue_host(handle),
            HV_DXG_CLEANUP_HWQUEUE, handle);
    }
    if (owner->gpuvas == NULL)
        owner->gpuva_count = 0;
    while (owner->gpuva_count > 0) {
        struct hvdxg_tracked_gpuva g =
            owner->gpuvas[--owner->gpuva_count];
        int gpuva_ret;

        hvdxg_wait_gpuva_fence(&g);
        gpuva_ret = hvdxg_free_gpuva_host(g.adapter, g.base, g.size);
        /*
         * Some D3D12 failure paths appear to invalidate the VA mapping before
         * the process close path runs. Explicit FREEGPUVA still reports that
         * error; cleanup treats it as already released and continues.
         */
        if (gpuva_ret == -EINVAL)
            gpuva_ret = 0;
        hvdxg_cleanup_note_ret(&ret, gpuva_ret,
                               HV_DXG_CLEANUP_GPUVA, (uint32)g.base);
    }
    if (owner->contexts == NULL)
        owner->context_count = 0;
    while (owner->context_count > 0) {
        uint32 handle = owner->contexts[--owner->context_count];
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_context_host(handle),
            HV_DXG_CLEANUP_CONTEXT, handle);
    }
    if (owner->allocations == NULL)
        owner->allocation_count = 0;
    while (owner->allocation_count > 0) {
        struct hvdxg_tracked_allocation a =
            owner->allocations[--owner->allocation_count];
        (void)hvdxg_unmap_tracked_allocation(&a);
        hvdxg_unpin_tracked_allocation(&a);
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_allocation_host(a.device, a.resource, a.allocation),
            HV_DXG_CLEANUP_ALLOCATION,
            a.allocation != 0 ? a.allocation : a.resource);
    }
    if (owner->resources == NULL)
        owner->resource_count = 0;
    while (owner->resource_count > 0)
        hvdxg_free_tracked_resource(&owner->resources[--owner->resource_count]);
    if (owner->paging_queues == NULL)
        owner->paging_queue_count = 0;
    while (owner->paging_queue_count > 0) {
        uint32 handle =
            owner->paging_queues[owner->paging_queue_count - 1].queue;
        uint32 sync = hvdxg_untrack_pagingqueue(owner, handle);

        if (sync != 0)
            hvdxg_untrack_sync(owner, sync);
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_pagingqueue_host(handle),
            HV_DXG_CLEANUP_PAGINGQUEUE, handle);
    }
    if (owner->sync_objects == NULL)
        owner->sync_object_count = 0;
    while (owner->sync_object_count > 0) {
        uint32 handle =
            owner->sync_objects[owner->sync_object_count - 1].sync;

        hvdxg_untrack_sync(owner, handle);
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_sync_host(handle),
            HV_DXG_CLEANUP_SYNC, handle);
    }
    if (owner->devices == NULL)
        owner->device_count = 0;
    while (owner->device_count > 0) {
        uint32 handle = owner->devices[--owner->device_count];
        hvdxg_cleanup_note_ret(
            &ret,
            hvdxg_destroy_device_host(handle),
            HV_DXG_CLEANUP_DEVICE, handle);
    }
    /*
     * Child handles are owned by the open file and are torn down above.
     * Keep the host process handle live for now: issuing DESTROYPROCESS here
     * makes later opens fail adapter enumeration on Hyper-V GPU-PV hosts.
     */
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
    while (owner->resource_count > 0)
        hvdxg_free_tracked_resource(&owner->resources[--owner->resource_count]);
    owner->gpuva_count = 0;
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
    if (owner->hwqueues != NULL)
        kvfree(owner->hwqueues);
    if (owner->paging_queues != NULL)
        kvfree(owner->paging_queues);
    if (owner->sync_objects != NULL)
        kvfree(owner->sync_objects);
    kvfree(owner);
}

static void hvdxg_note_unsupported_ioctl(uint32 cmd, uint32 device,
                                         uint32 handle, uint32 count)
{
    hvdxg.unsupported_last_cmd = cmd;
    hvdxg.unsupported_last_ret = -ENOTSUP;
    hvdxg.unsupported_last_device = device;
    hvdxg.unsupported_last_handle = handle;
    hvdxg.unsupported_last_count = count;
}

static uint32 hvdxg_queryadapter_private_max(void)
{
    uint32 command_max = HV_DXG_VM_BUS_PACKET_MAX -
                         sizeof(struct hvdxg_command_queryadapterinfo) + 1;
    uint32 result_max = HV_DXG_VM_BUS_PACKET_MAX -
                        sizeof(struct hvdxg_ntstatus);

    return command_max < result_max ? command_max : result_max;
}

static int hvdxg_ioctl_queryadapterinfo(uint64 arg)
{
    struct d3dkmt_queryadapterinfo req;
    uint8 *command_buf = NULL;
    uint8 *result_buf = NULL;
    struct hvdxg_command_queryadapterinfo *query;
    uint32 command_len;
    uint32 result_len;
    uint32 actual_len = 0;
    uint32 private_max = hvdxg_queryadapter_private_max();
    uint8 *private_data;
    int ret;

    ret = hvdxg_d3dkmt_ensure();
    if (ret != 0)
        return ret;
    if (either_copyin(&req, 1, arg, sizeof(req)) < 0)
        return -EFAULT;

    hvdxg.queryadapter_last_type = (uint32)req.type;
    hvdxg.queryadapter_last_size = req.private_data_size;
    hvdxg.queryadapter_last_len = 0;
    hvdxg.queryadapter_last_ret = 0;
    hvdxg.queryadapter_last_status = 0;
    if (req.adapter.v != hvdxg.host_adapter_handle ||
        req.private_data == 0 || req.private_data_size == 0 ||
        req.private_data_size > private_max) {
        ret = -EINVAL;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        return ret;
    }

    if (req.type == _KMTQAITYPE_UMDRIVERNAME) {
        uint32 version = 0;
        const char *umd_path = "/lib/libnvwgf2umx.so";

        if (hvdxg.adapter_vendor_id == HV_DXG_VENDOR_INTEL)
            umd_path = "/lib/libigd12umd64.so";
        else if (hvdxg.adapter_vendor_id == HV_DXG_VENDOR_NVIDIA)
            umd_path = "/lib/libnvwgf2umx.so";

        command_buf = kvmalloc(req.private_data_size);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            return ret;
        }
        memset(command_buf, 0, req.private_data_size);
        if (req.private_data_size >= sizeof(version) &&
            either_copyin(&version, 1, req.private_data,
                          sizeof(version)) == 0) {
            version = 3;
            memcpy(command_buf, &version, sizeof(version));
            if (req.private_data_size > sizeof(version)) {
                hvdxg_write_utf16_string(command_buf + sizeof(version),
                    req.private_data_size - sizeof(version),
                    umd_path);
            }
        }
        ret = either_copyout(1, req.private_data, command_buf,
                             req.private_data_size) < 0 ? -EFAULT : 0;
        hvdxg.queryadapter_last_len = req.private_data_size;
        hvdxg.queryadapter_last_status = 0;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, version);
        goto cleanup;
    }

    if (req.type == _KMTQAITYPE_DRIVER_DESCRIPTION ||
        req.type == _KMTQAITYPE_DRIVER_DESCRIPTION_RENDER) {
        command_buf = kvmalloc(req.private_data_size);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            return ret;
        }
        memset(command_buf, 0, req.private_data_size);
        hvdxg_write_utf16_string(command_buf, req.private_data_size,
                                 "Microsoft Hyper-V GPU-PV Render Driver");
        ret = either_copyout(1, req.private_data, command_buf,
                             req.private_data_size) < 0 ? -EFAULT : 0;
        hvdxg.queryadapter_last_len = req.private_data_size;
        hvdxg.queryadapter_last_status = 0;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }

    if (req.type == _KMTQAITYPE_PHYSICALADAPTERCOUNT) {
        uint32 count = 1;

        if (req.private_data_size < sizeof(count)) {
            ret = -EINVAL;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            return ret;
        }
        ret = either_copyout(1, req.private_data, &count, sizeof(count)) < 0 ?
              -EFAULT : 0;
        hvdxg.queryadapter_last_len = sizeof(count);
        hvdxg.queryadapter_last_status = 0;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, count);
        goto cleanup;
    }

    if (req.type == _KMTQAITYPE_QUERYREGISTRY) {
        static const char *const dxcore_attrs[] = {
            "{B69EB219-3DED-4464-979F-A00BD4687006}",
            "{0C9ECE4D-2F6E-4F01-8C96-E89E331B47B1}",
            "{8C47866B-7583-450D-F0F0-6BADA895AF4B}",
            "{248E2800-A793-4724-ABAA-23A6DE1BE090}",
            "{B71B0D41-1088-422F-A27C-0250B7D3A988}",
            "{8EB2C848-82F6-4B49-AA87-AECFCF0174C6}",
        };
        struct d3dddi_queryregistry_info *info;
        uint32 output_off = D3DDDI_QUERYREGISTRY_OUTPUT_OFFSET;
        uint32 output_cap;
        uint32 output_size = 0;
        int known_value = 0;

        command_buf = kvmalloc(req.private_data_size);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            return ret;
        }
        if (req.private_data_size < output_off) {
            ret = -EINVAL;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            goto cleanup;
        }
        if (either_copyin(command_buf, 1, req.private_data,
                          req.private_data_size) < 0) {
            ret = -EFAULT;
            hvdxg.queryadapter_last_ret = ret;
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, 0);
            goto cleanup;
        }
        info = (struct d3dddi_queryregistry_info *)command_buf;
        output_cap = req.private_data_size - output_off;
        hvdxg.queryregistry_last_query_type = (uint32)info->query_type;
        hvdxg.queryregistry_last_flags = info->query_flags.value;
        hvdxg.queryregistry_last_value_type = info->value_type;
        hvdxg.queryregistry_last_phys = info->physical_adapter_index;
        hvdxg.queryregistry_last_output_size = info->output_value_size;
        hvdxg.queryregistry_last_status =
            D3DDDI_QUERYREGISTRY_STATUS_FAIL;
        hvdxg.queryregistry_last_name0 =
            ((uint32)info->value_name[1] << 16) | info->value_name[0];
        hvdxg.queryregistry_last_name1 =
            ((uint32)info->value_name[3] << 16) | info->value_name[2];
        hvdxg_utf16_to_ascii(hvdxg.queryregistry_last_name,
                              sizeof(hvdxg.queryregistry_last_name),
                              info->value_name);

        if (info->query_type == D3DDDI_QUERYREGISTRY_ADAPTERKEY &&
            info->value_type == 7 &&
            (hvdxg_utf16_ascii_equals(info->value_name,
                                      "DXCoreAttributes") ||
             (info->value_name[0] == 'D' &&
              info->value_name[1] == 'X' &&
              info->value_name[2] == 'C' &&
              info->value_name[3] == 'o'))) {
            known_value = 1;
            output_size = hvdxg_utf16_multisz_size(dxcore_attrs,
                sizeof(dxcore_attrs) / sizeof(dxcore_attrs[0]));
        } else if (info->query_type == D3DDDI_QUERYREGISTRY_SERVICEKEY &&
                   info->value_type == 4 &&
                   hvdxg_utf16_ascii_equals(info->value_name,
                                            "EnableVGPUIndicator")) {
            known_value = 2;
            output_size = sizeof(uint32);
        }

        if (known_value && output_cap >= output_size) {
            if (known_value == 1) {
                hvdxg_write_utf16_multisz(command_buf + output_off,
                    dxcore_attrs,
                    sizeof(dxcore_attrs) / sizeof(dxcore_attrs[0]));
            } else {
                *(uint32 *)(command_buf + output_off) = 1;
            }
            info->output_value_size = output_size;
            info->status = D3DDDI_QUERYREGISTRY_STATUS_SUCCESS;
        } else if (known_value) {
            info->output_value_size = output_size;
            info->status = D3DDDI_QUERYREGISTRY_STATUS_BUFFER_OVERFLOW;
        } else {
            info->output_value_size = 0;
            info->status = D3DDDI_QUERYREGISTRY_STATUS_FAIL;
        }
        hvdxg.queryregistry_last_output_size = info->output_value_size;
        hvdxg.queryregistry_last_status = info->status;
        ret = either_copyout(1, req.private_data, command_buf,
                             req.private_data_size) < 0 ? -EFAULT : 0;
        hvdxg.queryadapter_last_len = req.private_data_size;
        hvdxg.queryadapter_last_status = 0;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, info->status);
        goto cleanup;
    }

    command_len = sizeof(*query) + req.private_data_size - 1;
    result_len = req.private_data_size + sizeof(struct hvdxg_ntstatus);
    command_buf = kvmalloc(command_len);
    result_buf = kvmalloc(result_len);
    if (command_buf == NULL || result_buf == NULL) {
        ret = -ENOMEM;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }
    memset(command_buf, 0, command_len);
    query = (struct hvdxg_command_queryadapterinfo *)command_buf;
    hvdxg_command_vgpu_init_process(&query->hdr,
                                    HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO,
                                    hvdxg.dxg_process);
    query->query_type = (uint32)req.type;
    query->private_data_size = req.private_data_size;
    if (either_copyin(query->private_data, 1, req.private_data,
                      req.private_data_size) < 0) {
        ret = -EFAULT;
        hvdxg.queryadapter_last_ret = ret;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret, 0);
        goto cleanup;
    }
    memset(result_buf, 0, result_len);
    ret = hvdxg_send_sync_vgpu(query, command_len, result_buf, result_len,
                               &actual_len);
    hvdxg.queryadapter_last_len = actual_len;
    hvdxg.queryadapter_last_ret = ret;
    if (ret != 0) {
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret,
            (uint32)hvdxg.queryadapter_last_status);
        goto cleanup;
    }
    if (actual_len >= req.private_data_size) {
        hvdxg.queryadapter_last_status = 0;
        private_data = result_buf;
    } else if (actual_len >= req.private_data_size +
                            sizeof(struct hvdxg_ntstatus)) {
        struct hvdxg_ntstatus *status =
            (struct hvdxg_ntstatus *)result_buf;
        hvdxg.queryadapter_last_status = status->v;
        ret = hvdxg_ntstatus_to_errno(*status);
        hvdxg.queryadapter_last_ret = ret;
        if (ret < 0) {
            hvdxg_note_queryadapter_history((uint32)req.type,
                req.private_data_size, ret, (uint32)status->v);
            goto cleanup;
        }
        private_data = result_buf + sizeof(struct hvdxg_ntstatus);
    } else {
        ret = -EOVERFLOW;
        hvdxg.queryadapter_last_ret = ret;
        if (actual_len >= sizeof(struct hvdxg_ntstatus))
            hvdxg.queryadapter_last_status =
                ((struct hvdxg_ntstatus *)result_buf)->v;
        hvdxg_note_queryadapter_history((uint32)req.type,
            req.private_data_size, ret,
            (uint32)hvdxg.queryadapter_last_status);
        goto cleanup;
    }
    if ((req.type == _KMTQAITYPE_ADAPTERTYPE ||
         req.type == _KMTQAITYPE_ADAPTERTYPE_RENDER) &&
        req.private_data_size >= sizeof(struct d3dkmt_adaptertype)) {
        struct d3dkmt_adaptertype *adapter_type =
            (struct d3dkmt_adaptertype *)private_data;
        /*
         * Hyper-V GPU-PV hosts in this VM report a transport-ready adapter
         * with render_supported clear when the partition's current compute
         * budget is zero. WSL/DXCore consumers treat adapter type as part of
         * adapter discovery, so expose the vGPU as a render-capable
         * paravirtualized adapter here while fb.c continues to gate real
         * OpenGL submit on validated context/submit success.
         */
        adapter_type->render_supported = 1;
        adapter_type->paravirtualized = 1;
        adapter_type->display_supported = 0;
        adapter_type->post_device = 0;
        adapter_type->indirect_display_device = 0;
        adapter_type->acg_supported = 0;
        adapter_type->support_set_timings_from_vidpn = 0;
    }
    if ((uint32)req.type == HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID)
        hvdxg_note_adapter_hardware_id(private_data, req.private_data_size);
    ret = either_copyout(1, req.private_data, private_data,
                         req.private_data_size) < 0 ? -EFAULT : 0;
    hvdxg.queryadapter_last_ret = ret;
    hvdxg_note_queryadapter_history((uint32)req.type,
        req.private_data_size, ret,
        (uint32)hvdxg.queryadapter_last_status);

cleanup:
    if (command_buf)
        kvfree(command_buf);
    if (result_buf)
        kvfree(result_buf);
    return ret;
}

static int hvdxg_ioctl_common(cdev_t *cdev, uint64 cmd, void *arg,
                              struct hvdxg_open_state *owner)
{
    (void)cdev;
    int ret = -EINVAL;
    uint64 start_ticks = r_time();

    hvdxg.ioctl_count++;
    ret = hvdxg_bind_open_process(owner);
    if (ret != 0) {
        hvdxg.ioctl_last_ret = ret;
        hvdxg_note_ioctl_timing(cmd, r_time() - start_ticks);
        return ret;
    }

    switch ((uint32)cmd) {
    case LX_DXENUMADAPTERS2: {
        struct d3dkmt_enumadapters2 req;
        struct d3dkmt_adapterinfo info;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapters == 0 || req.num_adapters == 0) {
            req.num_adapters = 1;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
            break;
        }
        if (req.num_adapters > D3DKMT_ADAPTERS_MAX) {
            ret = -EINVAL;
            break;
        }
        memset(&info, 0, sizeof(info));
        info.adapter_handle.v = hvdxg.host_adapter_handle;
        info.adapter_luid.a = hvdxg.adapter_luid.a;
        info.adapter_luid.b = hvdxg.adapter_luid.b;
        req.num_adapters = 1;
        if (either_copyout(1, req.adapters, &info, sizeof(info)) < 0 ||
            either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
            ret = -EFAULT;
        else
            ret = 0;
        break;
    }

    case LX_DXENUMADAPTERS3: {
        struct d3dkmt_enumadapters3 req;
        struct d3dkmt_adapterinfo info;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapters == 0 || req.adapter_count == 0) {
            req.adapter_count = 1;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
            break;
        }
        if (req.adapter_count > D3DKMT_ADAPTERS_MAX) {
            ret = -EINVAL;
            break;
        }
        memset(&info, 0, sizeof(info));
        info.adapter_handle.v = hvdxg.host_adapter_handle;
        info.adapter_luid.a = hvdxg.adapter_luid.a;
        info.adapter_luid.b = hvdxg.adapter_luid.b;
        req.adapter_count = 1;
        if (either_copyout(1, req.adapters, &info, sizeof(info)) < 0 ||
            either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
            ret = -EFAULT;
        else
            ret = 0;
        break;
    }

    case LX_DXOPENADAPTERFROMLUID: {
        struct d3dkmt_openadapterfromluid req;
        struct hvdxg_winluid luid;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        luid.a = req.adapter_luid.a;
        luid.b = req.adapter_luid.b;
        if (!hvdxg_luid_equal(luid, hvdxg.adapter_luid)) {
            ret = -EINVAL;
            break;
        }
        req.adapter_handle.v = hvdxg.host_adapter_handle;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        break;
    }

    case LX_DXCREATEDEVICE: {
        struct d3dkmt_createdevice req;
        struct hvdxg_command_createdevice create;
        struct hvdxg_command_createdevice_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapter.v != hvdxg.host_adapter_handle) {
            ret = -EINVAL;
            break;
        }
        memset(&create, 0, sizeof(create));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&create.hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEDEVICE,
                                        hvdxg.dxg_process);
        create.flags = req.flags;
        create.cdd_device = req.flags.gdi_device ? 1 : 0;
        create.error_code = (uint64)&hvdxg.device_state_counter;
        ret = hvdxg_send_sync_vgpu(&create, sizeof(create), &result,
                                   sizeof(result), &actual_len);
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.device.v == 0) {
            ret = -EIO;
            break;
        }
        req.device.v = result.device.v;
        hvdxg.last_device_handle = req.device.v;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            if (hvdxg_track_u32_grow(&owner->devices,
                                      &owner->device_count,
                                      &owner->device_capacity,
                                      req.device.v) != 0) {
                (void)hvdxg_destroy_device_host(req.device.v);
                ret = -ENOMEM;
            }
        }
        break;
    }

    case LX_DXDESTROYDEVICE: {
        struct d3dkmt_destroydevice req;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        ret = hvdxg_destroy_device_host(req.device.v);
        if (ret == 0 && hvdxg.last_device_handle == req.device.v)
            hvdxg.last_device_handle = 0;
        if (ret == 0 && owner != NULL)
            hvdxg_untrack_u32(owner->devices, &owner->device_count,
                              req.device.v);
        break;
    }

    case LX_DXCREATEALLOCATION: {
        struct d3dkmt_createallocation req;
        struct d3dkmt_createallocationflags requested_flags;
        struct d3dkmt_createstandardallocation standard_alloc;
        struct d3dddi_allocationinfo2 alloc_info[HV_DXG_ALLOCATION_MAX];
        uint32 alloc_priv_capacity[HV_DXG_ALLOCATION_MAX];
        uint64 *sysmem_pages[HV_DXG_ALLOCATION_MAX];
        uint32 sysmem_page_count[HV_DXG_ALLOCATION_MAX];
        uint8 *command_buf = NULL;
        uint8 *result_buf = NULL;
        struct hvdxg_command_createallocation *create =
            NULL;
        struct hvdxg_command_createallocation_allocinfo *wire_alloc;
        struct hvdxg_command_createallocation_return *result =
            NULL;
        uint8 *private_data;
        uint8 *alloc_private_data;
        uint8 *alloc_private_data_base = NULL;
        uint8 *standard_alloc_priv_data = NULL;
        uint8 *standard_res_priv_data = NULL;
        uint8 *resource_priv_data = NULL;
        uint32 standard_alloc_priv_data_size = 0;
        uint32 standard_res_priv_data_size = 0;
        uint32 total_alloc_private = 0;
        uint32 tracked_alloc_private = 0;
        uint32 tracked_resource_private_size = 0;
        uint32 total_private;
        uint32 command_len;
        uint32 result_min_len;
        uint32 result_len;
        uint32 actual_len = 0;
        uint32 requested_resource = 0;
        int host_allocation_created = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        requested_flags = req.flags;
        requested_resource = req.resource.v;
        if (req.device.v == 0 || req.alloc_count == 0 ||
            req.alloc_count > HV_DXG_ALLOCATION_MAX ||
            req.allocation_info == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        if (req.flags.standard_allocation) {
            if (req.standard_allocation == 0 || req.priv_drv_data_size != 0 ||
                req.alloc_count != 1) {
                ret = -EINVAL;
                break;
            }
            if (either_copyin(&standard_alloc, 1, req.standard_allocation,
                              sizeof(standard_alloc)) < 0) {
                ret = -EFAULT;
                break;
            }
            if (standard_alloc.existing_heap_data.size == 0 ||
                (standard_alloc.existing_heap_data.size & (PGSIZE - 1)) != 0) {
                ret = -EINVAL;
                break;
            }
            if (standard_alloc.type == _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP) {
                if (!req.flags.existing_sysmem) {
                    ret = -EINVAL;
                    break;
                }
            } else if (standard_alloc.type == _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER) {
                if (req.flags.existing_sysmem) {
                    ret = -EINVAL;
                    break;
                }
            } else {
                ret = -EINVAL;
                break;
            }
            req.priv_drv_data_size = sizeof(standard_alloc);
        }
        if (req.private_runtime_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            req.priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            (req.private_runtime_data_size != 0 &&
             req.private_runtime_data == 0) ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0 &&
             !req.flags.standard_allocation)) {
            ret = -EINVAL;
            break;
        }
        if (either_copyin(alloc_info, 1, req.allocation_info,
                          req.alloc_count * sizeof(alloc_info[0])) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.allocation_last_priv_size = alloc_info[0].priv_drv_data_size;
        hvdxg.allocation_last_flags = alloc_info[0].flags.value;
        hvdxg.allocation_last_sysmem = alloc_info[0].sysmem;
        hvdxg.allocation_last_priority = alloc_info[0].unused;
        hvdxg.existing_sysmem_last_pages = 0;
        hvdxg.existing_sysmem_last_pin_ret = 0;
        hvdxg.existing_sysmem_last_set_ret = 0;
        hvdxg.allocation_last_in_priv_head_len = 0;
        hvdxg.allocation_last_out_priv_head_len = 0;
        memset(hvdxg.allocation_last_in_priv_head, 0,
               sizeof(hvdxg.allocation_last_in_priv_head));
        memset(hvdxg.allocation_last_out_priv_head, 0,
               sizeof(hvdxg.allocation_last_out_priv_head));
        memset(alloc_priv_capacity, 0, sizeof(alloc_priv_capacity));
        memset(sysmem_pages, 0, sizeof(sysmem_pages));
        memset(sysmem_page_count, 0, sizeof(sysmem_page_count));
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (alloc_info[i].sysmem != 0 &&
                (alloc_info[i].sysmem & (PGSIZE - 1)) != 0) {
                ret = -EINVAL;
                break;
            }
            if ((alloc_info[0].sysmem == 0) !=
                (alloc_info[i].sysmem == 0)) {
                ret = -EINVAL;
                break;
            }
            if (req.flags.standard_allocation &&
                alloc_info[i].priv_drv_data_size != 0) {
                ret = -EINVAL;
                break;
            }
            alloc_priv_capacity[i] = alloc_info[i].priv_drv_data_size;
            if (alloc_info[i].priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
                (alloc_info[i].priv_drv_data_size != 0 &&
                 alloc_info[i].priv_drv_data == 0)) {
                ret = -EINVAL;
                break;
            }
            total_alloc_private += alloc_info[i].priv_drv_data_size;
            if (total_alloc_private > HV_DXG_VM_BUS_PACKET_MAX) {
                ret = -EOVERFLOW;
                break;
            }
        }
        if (ret != 0)
            break;
        if (req.flags.standard_allocation) {
            ret = hvdxg_get_standard_alloc_priv_data(
                req.device.v, &standard_alloc,
                &standard_alloc_priv_data_size, &standard_alloc_priv_data,
                &standard_res_priv_data_size, &standard_res_priv_data);
            if (ret != 0)
                goto createallocation_done;
            resource_priv_data = standard_res_priv_data;
            tracked_resource_private_size = standard_res_priv_data_size;
        } else {
            ret = hvdxg_copy_user_bytes(&resource_priv_data,
                                        req.priv_drv_data,
                                        req.priv_drv_data_size);
            if (ret != 0)
                goto createallocation_done;
            tracked_resource_private_size = req.priv_drv_data_size;
        }
        total_private = req.private_runtime_data_size +
                        req.priv_drv_data_size + total_alloc_private;
        if (total_private > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            goto createallocation_done;
        }

        command_len = sizeof(*create) +
                      req.alloc_count * sizeof(*wire_alloc) +
                      total_private;
        result_min_len = sizeof(*result) +
                         (req.alloc_count - 1) *
                             sizeof(struct hvdxg_command_allocinfo_return);
        result_len = result_min_len + total_alloc_private;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX ||
            result_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            goto createallocation_done;
        }
        command_buf = kvmalloc(command_len);
        result_buf = kvmalloc(result_len);
        if (command_buf == NULL || result_buf == NULL) {
            ret = -ENOMEM;
            goto createallocation_done;
        }
        memset(command_buf, 0, command_len);
        memset(result_buf, 0, result_len);
        create = (struct hvdxg_command_createallocation *)command_buf;
        result = (struct hvdxg_command_createallocation_return *)result_buf;
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEALLOCATION,
                                        hvdxg.dxg_process);
        create->device.v = req.device.v;
        create->resource.v = req.resource.v;
        create->private_runtime_data_size = req.private_runtime_data_size;
        create->priv_drv_data_size = req.priv_drv_data_size;
        create->alloc_count = req.alloc_count;
        create->flags = req.flags;
        create->private_runtime_resource_handle =
            req.private_runtime_resource_handle;
        create->make_resident = 0;
        if (alloc_info[0].sysmem != 0)
            create->flags.existing_sysmem = 1;

        wire_alloc = (struct hvdxg_command_createallocation_allocinfo *)&create[1];
        private_data = (uint8 *)wire_alloc +
                       req.alloc_count * sizeof(wire_alloc[0]);
        if (req.private_runtime_data_size != 0) {
            if (either_copyin(private_data, 1, req.private_runtime_data,
                              req.private_runtime_data_size) < 0) {
                ret = -EFAULT;
                goto createallocation_done;
            }
            private_data += req.private_runtime_data_size;
        }
        if (req.flags.standard_allocation) {
            memmove(private_data, &standard_alloc, sizeof(standard_alloc));
            private_data += sizeof(standard_alloc);
        } else if (req.priv_drv_data_size != 0) {
            if (either_copyin(private_data, 1, req.priv_drv_data,
                              req.priv_drv_data_size) < 0) {
                ret = -EFAULT;
                goto createallocation_done;
            }
            private_data += req.priv_drv_data_size;
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            wire_alloc[i].flags = alloc_info[i].flags.value;
            wire_alloc[i].priv_drv_data_size = alloc_info[i].priv_drv_data_size;
            wire_alloc[i].vidpn_source_id = alloc_info[i].vidpn_source_id;
            if (alloc_info[i].priv_drv_data_size != 0) {
                if (either_copyin(private_data, 1, alloc_info[i].priv_drv_data,
                                  alloc_info[i].priv_drv_data_size) < 0) {
                    ret = -EFAULT;
                    break;
                }
                if (i == 0)
                    hvdxg_save_priv_head(hvdxg.allocation_last_in_priv_head,
                                         sizeof(hvdxg.allocation_last_in_priv_head),
                                         &hvdxg.allocation_last_in_priv_head_len,
                                         private_data,
                                         alloc_info[i].priv_drv_data_size);
                private_data += alloc_info[i].priv_drv_data_size;
            }
        }
        if (ret != 0)
            goto createallocation_done;

        ret = hvdxg_send_sync_vgpu(create, command_len, result,
                                   result_len, &actual_len);
        hvdxg.allocation_last_len = actual_len;
        hvdxg.allocation_last_ret = ret;
        hvdxg.allocation_last_count = req.alloc_count;
        if (ret != 0) {
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v, 0, 0,
                                          req.alloc_count, total_private);
            goto createallocation_done;
        }
        if (actual_len < result_min_len ||
            result->allocation_info[0].allocation.v == 0) {
            ret = -EIO;
            hvdxg.allocation_last_ret = ret;
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v, 0, 0,
                                          req.alloc_count, total_private);
            goto createallocation_done;
        }
        host_allocation_created = 1;
        req.flags = result->flags;
        req.resource.v = result->resource.v;
        req.global_share.v = result->global_share.v;
        alloc_private_data = result_buf + result_min_len;
        alloc_private_data_base = alloc_private_data;
        for (uint32 i = 0; i < req.alloc_count; i++) {
            uint32 host_private_size =
                result->allocation_info[i].priv_drv_data_size;
            alloc_info[i].allocation.v =
                result->allocation_info[i].allocation.v;
            alloc_info[i].priv_drv_data_size = host_private_size;
            if (host_private_size != 0) {
                if (host_private_size > alloc_priv_capacity[i] ||
                    alloc_info[i].priv_drv_data == 0 ||
                    (uint64)(alloc_private_data - result_buf) +
                        host_private_size > actual_len) {
                    ret = -EOVERFLOW;
                    break;
                }
                if (either_copyout(1, alloc_info[i].priv_drv_data,
                                   alloc_private_data,
                                   host_private_size) < 0) {
                    ret = -EFAULT;
                    break;
                }
                if (i == 0)
                    hvdxg_save_priv_head(hvdxg.allocation_last_out_priv_head,
                                         sizeof(hvdxg.allocation_last_out_priv_head),
                                         &hvdxg.allocation_last_out_priv_head_len,
                                         alloc_private_data,
                                         host_private_size);
                alloc_private_data += host_private_size;
            }
        }
        if (ret != 0) {
            hvdxg.allocation_last_ret = ret;
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v,
                                          alloc_info[0].allocation.v,
                                          result->allocation_info[0].allocation_size,
                                          req.alloc_count, total_private);
            goto createallocation_done;
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (alloc_info[i].sysmem != 0) {
                ret = hvdxg_pin_existing_sysmem(
                    alloc_info[i].sysmem,
                    result->allocation_info[i].allocation_size,
                    !req.flags.read_only,
                    &sysmem_pages[i],
                    &sysmem_page_count[i]);
                hvdxg.existing_sysmem_last_pages = sysmem_page_count[i];
                hvdxg.existing_sysmem_last_pin_ret = ret;
                if (ret != 0) {
                    hvdxg.allocation_last_ret = ret;
                    hvdxg_note_allocation_history(
                        actual_len, ret, req.device.v, req.resource.v,
                        alloc_info[i].allocation.v,
                        result->allocation_info[i].allocation_size,
                        req.alloc_count, total_private);
                    goto createallocation_done;
                }
                hvdxg.existing_sysmem_pin_successes++;
                hvdxg.existing_sysmem_total_pages += sysmem_page_count[i];
                ret = hvdxg_set_existing_sysmem_pages(
                    req.device.v, alloc_info[i].allocation.v,
                    sysmem_pages[i], sysmem_page_count[i]);
                hvdxg.existing_sysmem_last_set_ret = ret;
                if (ret != 0) {
                    hvdxg.allocation_last_ret = ret;
                    hvdxg_note_allocation_history(
                        actual_len, ret, req.device.v, req.resource.v,
                        alloc_info[i].allocation.v,
                        result->allocation_info[i].allocation_size,
                        req.alloc_count, total_private);
                    goto createallocation_done;
                }
                hvdxg.existing_sysmem_set_successes++;
            }
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            uint64 user_alloc_handle =
                req.allocation_info +
                (uint64)i * sizeof(struct d3dddi_allocationinfo2) +
                offsetof(struct d3dddi_allocationinfo2, allocation);
            if (either_copyout(1, user_alloc_handle,
                               &alloc_info[i].allocation,
                               sizeof(alloc_info[i].allocation)) < 0) {
                ret = -EFAULT;
                break;
            }
        }
        if (ret == 0 && req.flags.create_resource &&
            either_copyout(1,
                           (uint64)arg +
                               offsetof(struct d3dkmt_createallocation,
                                        resource),
                           &req.resource, sizeof(req.resource)) < 0) {
            ret = -EFAULT;
        }
        if (ret == 0 &&
            either_copyout(1,
                           (uint64)arg +
                               offsetof(struct d3dkmt_createallocation,
                                        global_share),
                           &req.global_share, sizeof(req.global_share)) < 0) {
            ret = -EFAULT;
        }
        if (ret != 0) {
            hvdxg.allocation_last_ret = ret;
            hvdxg_note_allocation_history(actual_len, ret, req.device.v,
                                          req.resource.v,
                                          alloc_info[0].allocation.v,
                                          result->allocation_info[0].allocation_size,
                                          req.alloc_count, total_private);
            goto createallocation_done;
        }
        hvdxg.last_device_handle = req.device.v;
        hvdxg.last_resource_handle = req.resource.v;
        hvdxg.last_allocation_handle = alloc_info[0].allocation.v;
        hvdxg.last_allocation_device = req.device.v;
        hvdxg.last_allocation_size = result->allocation_info[0].allocation_size;
        if (owner != NULL) {
            tracked_alloc_private = req.flags.standard_allocation ?
                                    standard_alloc_priv_data_size :
                                    total_alloc_private;
            hvdxg_track_resource(owner, &req, requested_flags, alloc_info, result,
                                 req.flags.standard_allocation ?
                                     standard_alloc_priv_data :
                                     alloc_private_data_base,
                                 tracked_alloc_private,
                                 resource_priv_data,
                                 tracked_resource_private_size);
            for (uint32 i = 0; i < req.alloc_count; i++) {
                hvdxg_track_allocation(owner, req.device.v, req.resource.v,
                                       alloc_info[i].allocation.v,
                                       result->allocation_info[i].allocation_size,
                                       result->allocation_info[i].allocation_flags,
                                       alloc_info[i].sysmem,
                                       sysmem_pages[i],
                                       sysmem_page_count[i]);
                sysmem_pages[i] = NULL;
                sysmem_page_count[i] = 0;
                hvdxg_note_allocation_history(
                    actual_len, ret, req.device.v, req.resource.v,
                    alloc_info[i].allocation.v,
                    result->allocation_info[i].allocation_size,
                    req.alloc_count, total_private);
            }
        }
createallocation_done:
        if (ret != 0 && host_allocation_created) {
            int unwind_ret =
                hvdxg_destroy_createallocation_result(
                    req.device.v, requested_resource, result,
                    req.alloc_count);

            if (unwind_ret != 0 && hvdxg.allocation_last_ret == ret)
                hvdxg.allocation_last_ret = unwind_ret;
        }
        if (command_buf != NULL)
            kvfree(command_buf);
        if (result_buf != NULL)
            kvfree(result_buf);
        if (standard_alloc_priv_data != NULL)
            kvfree(standard_alloc_priv_data);
        if (standard_res_priv_data != NULL)
            kvfree(standard_res_priv_data);
        if (!req.flags.standard_allocation && resource_priv_data != NULL)
            kvfree(resource_priv_data);
        for (uint32 i = 0; i < HV_DXG_ALLOCATION_MAX; i++)
            hvdxg_unpin_existing_sysmem_pages(sysmem_pages[i],
                                              sysmem_page_count[i]);
        break;
    }

    case LX_DXDESTROYALLOCATION2: {
        struct d3dkmt_destroyallocation2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_destroyallocation) +
                          HV_DXG_ALLOCATION_MAX *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_destroyallocation *destroy =
            (struct hvdxg_command_destroyallocation *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.alloc_count > HV_DXG_ALLOCATION_MAX ||
            (req.alloc_count != 0 && req.allocations == 0)) {
            ret = -EINVAL;
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy->hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYALLOCATION,
                                        hvdxg.dxg_process);
        destroy->device.v = req.device.v;
        destroy->resource.v = req.resource.v;
        destroy->alloc_count = req.alloc_count;
        destroy->flags = req.flags;
        if (req.alloc_count != 0 &&
            either_copyin(destroy->allocations, 1, req.allocations,
                          req.alloc_count * sizeof(destroy->allocations[0])) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.alloc_count == 0) {
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        } else {
            for (uint32 i = 0; i < req.alloc_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                                req.resource.v,
                                                destroy->allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        command_len = sizeof(*destroy) +
                      req.alloc_count * sizeof(destroy->allocations[0]);
        ret = hvdxg_send_sync_vgpu(destroy, command_len, &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.destroyalloc_last_len = actual_len;
        hvdxg.destroyalloc_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            if (req.alloc_count == 0) {
                hvdxg_untrack_resource(owner, req.device.v, req.resource.v);
                hvdxg_untrack_allocation(owner, req.device.v, req.resource.v,
                                         0);
            } else {
                for (uint32 i = 0; i < req.alloc_count; i++)
                    hvdxg_untrack_allocation(owner, req.device.v,
                                             req.resource.v,
                                             destroy->allocations[i].v);
                if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                                req.resource.v, 0))
                    hvdxg_untrack_resource(owner, req.device.v,
                                           req.resource.v);
            }
        }
        break;
    }

    case LX_DXGETDEVICESTATE: {
        struct d3dkmt_getdevicestate req;
        struct hvdxg_command_getdevicestate getstate;
        struct hvdxg_command_getdevicestate_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        memset(&getstate, 0, sizeof(getstate));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&getstate.hdr,
                                        HV_DXGK_VMBCOMMAND_GETDEVICESTATE,
                                        hvdxg.dxg_process);
        getstate.args = req;
        ret = hvdxg_send_sync_vgpu(&getstate, sizeof(getstate), &result,
                                   sizeof(result), &actual_len);
        if (ret != 0)
            break;
        if (actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
            break;
        }
        ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0)
            break;
        req = result.args;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        break;
    }

    case LX_DXCREATEPAGINGQUEUE: {
        struct d3dkmt_createpagingqueue req;
        struct hvdxg_command_createpagingqueue create;
        struct hvdxg_command_createpagingqueue_return result;
        uint32 actual_len = 0;
        uint64 fence_kva = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        memset(&create, 0, sizeof(create));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&create.hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEPAGINGQUEUE,
                                        hvdxg.dxg_process);
        create.args = req;
        create.args.paging_queue.v = 0;
        create.args.sync_object.v = 0;
        create.args.fence_cpu_virtual_address = 0;
        ret = hvdxg_send_sync_vgpu(&create, sizeof(create), &result,
                                   sizeof(result), &actual_len);
        hvdxg.pagingqueue_last_len = actual_len;
        hvdxg.pagingqueue_last_ret = ret;
        hvdxg.pagingqueue_last_queue = result.paging_queue.v;
        hvdxg.pagingqueue_last_sync = result.sync_object.v;
        hvdxg.pagingqueue_last_fence_pa = result.fence_storage_physical_address;
        hvdxg.pagingqueue_last_fence_off = result.fence_storage_offset;
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.paging_queue.v == 0) {
            ret = -EIO;
            break;
        }
        req.paging_queue.v = result.paging_queue.v;
        req.sync_object.v = result.sync_object.v;
        req.fence_cpu_virtual_address = hvdxg_map_iospace_user(
            result.fence_storage_physical_address, PGSIZE, 0);
        fence_kva = hvdxg_map_iospace_kernel(
            result.fence_storage_physical_address, PGSIZE);
        if (req.fence_cpu_virtual_address == 0) {
            ret = -ENOMEM;
            break;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            hvdxg_track_pagingqueue(owner, req.device.v, req.paging_queue.v,
                                    req.sync_object.v,
                                    result.fence_storage_physical_address);
            hvdxg_track_sync(owner, req.device.v, req.sync_object.v,
                             _D3DDDI_MONITORED_FENCE,
                             0, result.sync_object.v,
                             req.fence_cpu_virtual_address, fence_kva);
        }
        break;
    }

    case LX_DXDESTROYPAGINGQUEUE: {
        struct d3dddi_destroypagingqueue req;
        struct hvdxg_command_destroypagingqueue destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE,
                                        hvdxg.dxg_process);
        destroy.paging_queue.v = req.paging_queue.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        if (ret == 0 && owner != NULL) {
            uint32 sync = hvdxg_untrack_pagingqueue(owner,
                                                    req.paging_queue.v);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
        }
        break;
    }

    case LX_DXMAKERESIDENT: {
        struct d3dddi_makeresident req;
        struct hvdxg_command_makeresident *make = NULL;
        struct hvdxg_command_makeresident_return result;
        uint32 actual_len = 0;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0 || req.alloc_count == 0 ||
            req.alloc_count > HV_DXG_RESIDENCY_ALLOCATION_MAX ||
            req.allocation_list == 0) {
            ret = -EINVAL;
            break;
        }
        command_len = sizeof(*make) +
                      (req.alloc_count - 1) * sizeof(make->allocations[0]);
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        make = kvmalloc(command_len);
        if (make == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(make, 0, command_len);
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&make->hdr,
                                        HV_DXGK_VMBCOMMAND_MAKERESIDENT,
                                        hvdxg.dxg_process);
        make->paging_queue.v = req.paging_queue.v;
        make->flags = req.flags;
        make->alloc_count = req.alloc_count;
        if (either_copyin(make->allocations, 1, req.allocation_list,
                          req.alloc_count * sizeof(make->allocations[0])) < 0) {
            ret = -EFAULT;
            goto makeresident_done;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            goto makeresident_done;
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, 0, 0,
                                            make->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            goto makeresident_done;
        /*
         * The NVIDIA D3D12 UMD under WSL emits the second-FBO residency
         * batch in ascending host-handle order.  Mesa on xv6 can hand us the
         * same color/depth pair in the opposite order, and the host rejects
         * that packet with STATUS_INVALID_PARAMETER.  Normalize the wire
         * packet so the host sees the WSL-observed order.
         */
        for (uint32 i = 1; i < req.alloc_count; i++) {
            struct hvdxg_d3dkmthandle key = make->allocations[i];
            uint32 j = i;

            while (j > 0 && make->allocations[j - 1].v > key.v) {
                make->allocations[j] = make->allocations[j - 1];
                j--;
            }
            make->allocations[j] = key;
        }
        /*
         * WSL leaves the VM-bus device field zero for MAKERESIDENT even
         * though the wire struct carries it.  Some hosts return a different
         * residency/fence path when a device is supplied here.
         */
        make->device.v = 0;
        ret = hvdxg_send_sync_vgpu(make, command_len, &result,
                                   sizeof(result), &actual_len);
        if (ret == 0 && actual_len >= sizeof(result))
            ret = hvdxg_ntstatus_to_errno(result.status);
        /*
         * Match WSL's make-resident contract: STATUS_PENDING is a
         * positive success code returned to user mode, and the paging
         * fence/trim outputs must still be copied back.  Mesa waits on
         * that fence before locking/touching the allocation.
         */
        if (ret == 0 || ret == 259) {
            int copy_ret = ret;

            req.paging_fence_value = result.paging_fence_value;
            req.num_bytes_to_trim = result.num_bytes_to_trim;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : copy_ret;
        }
makeresident_done:
        hvdxg.makeresident_last_len = actual_len;
        hvdxg.makeresident_last_ret = ret;
        hvdxg.makeresident_last_fence = req.paging_fence_value;
        hvdxg.makeresident_last_trim = req.num_bytes_to_trim;
        if (make != NULL)
            kvfree(make);
        break;
    }

    case LX_DXEVICT: {
        struct d3dkmt_evict req;
        struct hvdxg_command_evict *evict = NULL;
        struct hvdxg_command_evict_return result;
        uint32 actual_len = 0;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.alloc_count == 0 ||
            req.alloc_count > HV_DXG_RESIDENCY_ALLOCATION_MAX ||
            req.allocations == 0) {
            ret = -EINVAL;
            break;
        }
        command_len = sizeof(*evict) +
                      (req.alloc_count - 1) * sizeof(evict->allocations[0]);
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        evict = kvmalloc(command_len);
        if (evict == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(evict, 0, command_len);
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&evict->hdr,
                                        HV_DXGK_VMBCOMMAND_EVICT,
                                        hvdxg.dxg_process);
        evict->device.v = req.device.v;
        evict->flags = req.flags;
        evict->alloc_count = req.alloc_count;
        if (either_copyin(evict->allocations, 1, req.allocations,
                          req.alloc_count * sizeof(evict->allocations[0])) < 0) {
            ret = -EFAULT;
            goto evict_done;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            goto evict_done;
        }
        for (uint32 i = 0; i < req.alloc_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                            evict->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            goto evict_done;
        ret = hvdxg_send_sync_vgpu(evict, command_len, &result,
                                   sizeof(result), &actual_len);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.num_bytes_to_trim = result.num_bytes_to_trim;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
evict_done:
        hvdxg.evict_last_len = actual_len;
        hvdxg.evict_last_ret = ret;
        hvdxg.evict_last_trim = req.num_bytes_to_trim;
        if (evict != NULL)
            kvfree(evict);
        break;
    }

    case LX_DXMAPGPUVIRTUALADDRESS: {
        struct d3dddi_mapgpuvirtualaddress req;
        struct hvdxg_command_mapgpuvirtualaddress map;
        struct hvdxg_command_mapgpuvirtualaddress_return result;
        uint32 actual_len = 0;
        uint64 allocation_pages = 0;
        uint64 mapped_pages = 0;
        int map_ok = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0 || req.size_in_pages == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            break;
        }
        if (req.allocation.v != 0) {
            if (!hvdxg_owner_has_allocation(owner, 0, 0,
                                            req.allocation.v)) {
                ret = -EPERM;
                break;
            }
        } else {
            uint64 map_size = req.size_in_pages << 12;

            if ((map_size >> 12) != req.size_in_pages ||
                req.base_address == 0 ||
                (!hvdxg_owner_gpuva_contains(owner, req.paging_queue.v,
                                             req.base_address, map_size) &&
                 !hvdxg_owner_gpuva_contains(owner, 0, req.base_address,
                                             map_size))) {
                ret = -EPERM;
                break;
            }
        }
        memset(&map, 0, sizeof(map));
        memset(&result, 0, sizeof(result));
        hvdxg.mapgpuva_last_status = 0;
        hvdxg.mapgpuva_last_paging_queue = req.paging_queue.v;
        hvdxg.mapgpuva_last_allocation = req.allocation.v;
        hvdxg.mapgpuva_last_base = req.base_address;
        hvdxg.mapgpuva_last_min = req.minimum_address;
        hvdxg.mapgpuva_last_max = req.maximum_address;
        hvdxg.mapgpuva_last_size_pages = req.size_in_pages;
        hvdxg.mapgpuva_last_protection = req.protection.value;
        hvdxg.mapgpuva_last_driver_protection = req.driver_protection;
        hvdxg_command_vgpu_init_process(&map.hdr,
                                        HV_DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS,
                                        hvdxg.dxg_process);
        map.args = req;
        map.device.v = 0;
        mapped_pages = req.size_in_pages;
        ret = hvdxg_send_sync_vgpu(&map, sizeof(map), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.mapgpuva_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0 &&
            (uint32)hvdxg.mapgpuva_last_status == 0xC0000001U &&
            req.allocation.v != 0 &&
            hvdxg.last_allocation_handle == req.allocation.v &&
            hvdxg.last_allocation_size != 0) {
            allocation_pages =
                (hvdxg.last_allocation_size + PGSIZE - 1) >> 12;
            if (allocation_pages != 0 &&
                req.size_in_pages > allocation_pages) {
                struct d3dddi_mapgpuvirtualaddress retry_args = req;

                retry_args.size_in_pages = allocation_pages;
                retry_args.virtual_address = 0;
                retry_args.paging_fence_value = 0;
                memset(&map, 0, sizeof(map));
                memset(&result, 0, sizeof(result));
                hvdxg_command_vgpu_init_process(
                    &map.hdr, HV_DXGK_VMBCOMMAND_MAPGPUVIRTUALADDRESS,
                    hvdxg.dxg_process);
                map.args = retry_args;
                map.device.v = 0;
                actual_len = 0;
                mapped_pages = allocation_pages;
                ret = hvdxg_send_sync_vgpu(&map, sizeof(map), &result,
                                           sizeof(result), &actual_len);
                if (actual_len >= sizeof(result.status))
                    hvdxg.mapgpuva_last_status = result.status.v;
                if (ret == 0 && actual_len >= sizeof(result))
                    ret = hvdxg_ntstatus_to_errno(result.status);
            }
        }
        if (ret == 0 || ret == 259) {
            int copy_ret = ret;

            req.virtual_address = result.virtual_address;
            req.paging_fence_value = result.paging_fence_value;
            if (mapped_pages != req.size_in_pages)
                req.size_in_pages = mapped_pages;
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
                ret = -EFAULT;
            else {
                map_ok = req.virtual_address != 0;
                ret = copy_ret;
            }
        }
        hvdxg.mapgpuva_last_len = actual_len;
        hvdxg.mapgpuva_last_ret = ret;
        hvdxg.mapgpuva_last_va = req.virtual_address;
        hvdxg.mapgpuva_last_fence = req.paging_fence_value;
        hvdxg_note_mapgpuva_history(actual_len, ret,
                                    (uint32)hvdxg.mapgpuva_last_status,
                                    req.paging_queue.v, req.allocation.v,
                                    mapped_pages, req.virtual_address,
                                    req.paging_fence_value);
        if (map_ok && owner != NULL)
            hvdxg_track_gpuva(owner, hvdxg.host_adapter_handle,
                              req.virtual_address, mapped_pages << 12,
                              req.paging_fence_value,
                              hvdxg_owner_pagingqueue_fence_pa(
                                  owner, req.paging_queue.v));
        break;
    }

    case LX_DXSUBMITCOMMAND: {
        struct d3dkmt_submitcommand req;
        struct hvdxg_command_submitcommand *submit = NULL;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 history_size;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.submit_last_command_buffer = req.command_buffer;
        hvdxg.submit_last_command_length = req.command_length;
        hvdxg.submit_last_flags = req.flags.value;
        hvdxg.submit_last_priv_size = req.priv_drv_data_size;
        hvdxg.submit_last_context_count = req.broadcast_context_count;
        hvdxg.submit_last_context0 =
            req.broadcast_context_count != 0 ? req.broadcast_context[0].v : 0;
        hvdxg.submit_last_status = 0;
        if (req.broadcast_context_count == 0 ||
            req.broadcast_context_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
            req.num_primaries > D3DDDI_MAX_WRITTEN_PRIMARIES ||
            req.num_history_buffers > HV_DXG_HISTORY_BUFFER_MAX ||
            req.priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            (req.num_history_buffers != 0 && req.history_buffer_array == 0) ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        for (uint32 i = 0; i < req.broadcast_context_count; i++) {
            if (!hvdxg_owner_has_context(owner, req.broadcast_context[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        history_size = req.num_history_buffers *
                       sizeof(struct hvdxg_d3dkmthandle);
        command_len = sizeof(*submit) + history_size +
                      req.priv_drv_data_size;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        submit = kvmalloc(command_len);
        if (submit == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(submit, 0, command_len);
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&submit->hdr,
                                        HV_DXGK_VMBCOMMAND_SUBMITCOMMAND,
                                        hvdxg.dxg_process);
        submit->args = req;
        if (history_size != 0 &&
            either_copyin(&submit[1], 1, req.history_buffer_array,
                          history_size) < 0) {
            ret = -EFAULT;
            goto submitcommand_done;
        }
        if (req.priv_drv_data_size != 0 &&
            either_copyin((uint8 *)&submit[1] + history_size, 1,
                          req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto submitcommand_done;
        }
        hvdxg_wc_store_fence();
        ret = hvdxg_send_sync_vgpu(submit, command_len, &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.submit_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.submit_last_len = actual_len;
        hvdxg.submit_last_ret = ret;
submitcommand_done:
        if (submit != NULL)
            kvfree(submit);
        break;
    }

    case LX_DXSUBMITCOMMANDTOHWQUEUE: {
        struct d3dkmt_submitcommandtohwqueue req;
        struct hvdxg_command_submitcommandtohwqueue *submit = NULL;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 primaries_size;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.hwqueue.v == 0 ||
            req.num_primaries > D3DDDI_MAX_WRITTEN_PRIMARIES ||
            req.priv_drv_data_size > HV_DXG_VM_BUS_PACKET_MAX ||
            (req.num_primaries != 0 && req.written_primaries == 0) ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_hwqueue(owner, req.hwqueue.v)) {
            ret = -EPERM;
            break;
        }
        hvdxg.submithwqueue_last_queue = req.hwqueue.v;
        hvdxg.submithwqueue_last_fence_id =
            req.hwqueue_progress_fence_id;
        hvdxg.submithwqueue_last_command_length = req.command_length;
        hvdxg.submithwqueue_last_priv_size = req.priv_drv_data_size;
        hvdxg.submithwqueue_last_priv_head_len = 0;
        memset(hvdxg.submithwqueue_last_priv_head, 0,
               sizeof(hvdxg.submithwqueue_last_priv_head));
        primaries_size =
            req.num_primaries * sizeof(struct hvdxg_d3dkmthandle);
        command_len =
            sizeof(*submit) + primaries_size + req.priv_drv_data_size;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EOVERFLOW;
            break;
        }
        submit = kvmalloc(command_len);
        if (submit == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(submit, 0, command_len);
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &submit->hdr, HV_DXGK_VMBCOMMAND_SUBMITCOMMANDTOHWQUEUE,
            hvdxg.dxg_process);
        submit->args = req;
        if (primaries_size != 0 &&
            either_copyin(&submit[1], 1, req.written_primaries,
                          primaries_size) < 0) {
            ret = -EFAULT;
            goto submithwqueue_done;
        }
        if (req.priv_drv_data_size != 0 &&
            either_copyin((uint8 *)&submit[1] + primaries_size, 1,
                          req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto submithwqueue_done;
        }
        if (req.priv_drv_data_size != 0)
            hvdxg_save_priv_head(hvdxg.submithwqueue_last_priv_head,
                                 sizeof(hvdxg.submithwqueue_last_priv_head),
                                 &hvdxg.submithwqueue_last_priv_head_len,
                                 (uint8 *)&submit[1] + primaries_size,
                                 req.priv_drv_data_size);
        hvdxg_wc_store_fence();
        ret = hvdxg_send_sync_vgpu(submit, command_len, &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.submithwqueue_last_len = actual_len;
        hvdxg.submithwqueue_last_ret = ret;
submithwqueue_done:
        if (submit != NULL)
            kvfree(submit);
        break;
    }

    case LX_DXRESERVEGPUVIRTUALADDRESS: {
        struct d3dddi_reservegpuvirtualaddress req;
        struct hvdxg_command_reservegpuvirtualaddress reserve;
        struct hvdxg_command_reservegpuvirtualaddress_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if ((req.adapter.v != hvdxg.host_adapter_handle &&
             !hvdxg_owner_has_pagingqueue(owner, req.adapter.v)) ||
            req.size == 0) {
            ret = -EINVAL;
            break;
        }
        memset(&reserve, 0, sizeof(reserve));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &reserve.hdr, HV_DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        reserve.args = req;
        ret = hvdxg_send_sync_vgpu(&reserve, sizeof(reserve), &result,
                                   sizeof(result), &actual_len);
        hvdxg.gpuva_reserve_last_len = actual_len;
        hvdxg.gpuva_reserve_last_ret = ret;
        hvdxg.gpuva_reserve_last_va = result.virtual_address;
        hvdxg.gpuva_reserve_last_fence = result.paging_fence_value;
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.virtual_address == 0) {
            ret = -EIO;
            break;
        }
        req.virtual_address = result.virtual_address;
        req.paging_fence_value = result.paging_fence_value;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL)
            hvdxg_track_gpuva(owner, req.adapter.v, req.virtual_address,
                              req.size, req.paging_fence_value, 0);
        break;
    }

    case LX_DXFREEGPUVIRTUALADDRESS: {
        struct d3dkmt_freegpuvirtualaddress req;
        struct d3dkmt_freegpuvirtualaddress wire_req;
        struct hvdxg_command_freegpuvirtualaddress free_gpuva;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint64 tracked_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if ((req.adapter.v != hvdxg.host_adapter_handle &&
             !hvdxg_owner_has_gpuva(owner, req.adapter.v,
                                     req.base_address)) ||
            req.base_address == 0 || req.size == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_gpuva(owner, req.adapter.v, req.base_address) &&
            !hvdxg_owner_has_gpuva(owner, 0, req.base_address)) {
            ret = -EPERM;
            break;
        }
        wire_req = req;
        tracked_size = hvdxg_owner_gpuva_size(owner, req.adapter.v,
                                              req.base_address);
        if (tracked_size == 0)
            tracked_size = hvdxg_owner_gpuva_size(owner, 0,
                                                  req.base_address);
        if (tracked_size != 0 && tracked_size != req.size)
            wire_req.size = tracked_size;
        memset(&free_gpuva, 0, sizeof(free_gpuva));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &free_gpuva.hdr, HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        free_gpuva.args = wire_req;
        ret = hvdxg_send_sync_vgpu(&free_gpuva, sizeof(free_gpuva), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.gpuva_free_last_len = actual_len;
        hvdxg.gpuva_free_last_ret = ret;
        if (ret == 0 && owner != NULL)
            hvdxg_untrack_gpuva(owner, req.base_address);
        break;
    }

    case LX_DXFLUSHHEAPTRANSITIONS: {
        struct d3dkmt_flushheaptransitions req;
        struct hvdxg_command_flushheaptransitions flush;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapter.v != hvdxg.host_adapter_handle) {
            ret = -EINVAL;
            break;
        }
        memset(&flush, 0, sizeof(flush));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &flush.hdr, HV_DXGK_VMBCOMMAND_FLUSHHEAPTRANSITIONS,
            hvdxg.dxg_process);
        ret = hvdxg_send_sync_vgpu(&flush, sizeof(flush), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.cacheops_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.cacheops_last_len = actual_len;
        hvdxg.cacheops_last_ret = ret;
        hvdxg.cacheops_last_allocation = 0;
        break;
    }

    case LX_DXINVALIDATECACHE: {
        struct d3dkmt_invalidatecache req;
        struct hvdxg_command_invalidatecache invalidate;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                        req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        memset(&invalidate, 0, sizeof(invalidate));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &invalidate.hdr, HV_DXGK_VMBCOMMAND_INVALIDATECACHE,
            hvdxg.dxg_process);
        invalidate.device.v = req.device.v;
        invalidate.allocation.v = req.allocation.v;
        invalidate.offset = req.offset;
        invalidate.length = req.length;
        ret = hvdxg_send_sync_vgpu(&invalidate, sizeof(invalidate), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.cacheops_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.cacheops_last_len = actual_len;
        hvdxg.cacheops_last_ret = ret;
        hvdxg.cacheops_last_allocation = req.allocation.v;
        break;
    }

    case LX_DXSETCONTEXTSCHEDULINGPRIORITY:
    case LX_DXSETCONTEXTINPROCESSSCHEDULINGPRIORITY: {
        struct d3dkmt_setcontextschedulingpriority req;
        struct hvdxg_command_setcontextschedulingpriority set;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        memset(&set, 0, sizeof(set));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &set.hdr, HV_DXGK_VMBCOMMAND_SETCONTEXTSCHEDULINGPRIORITY,
            hvdxg.dxg_process);
        set.context.v = req.context.v;
        set.priority = req.priority;
        set.in_process =
            ((uint32)cmd == LX_DXSETCONTEXTINPROCESSSCHEDULINGPRIORITY);
        ret = hvdxg_send_sync_vgpu(&set, sizeof(set), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.schedprio_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.schedprio_last_len = actual_len;
        hvdxg.schedprio_last_ret = ret;
        hvdxg.schedprio_last_context = req.context.v;
        hvdxg.schedprio_last_value = req.priority;
        break;
    }

    case LX_DXGETCONTEXTSCHEDULINGPRIORITY:
    case LX_DXGETCONTEXTINPROCESSSCHEDULINGPRIORITY: {
        struct d3dkmt_getcontextschedulingpriority req;
        struct hvdxg_command_getcontextschedulingpriority get;
        struct hvdxg_command_getcontextschedulingpriority_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        memset(&get, 0, sizeof(get));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &get.hdr, HV_DXGK_VMBCOMMAND_GETCONTEXTSCHEDULINGPRIORITY,
            hvdxg.dxg_process);
        get.context.v = req.context.v;
        get.in_process =
            ((uint32)cmd == LX_DXGETCONTEXTINPROCESSSCHEDULINGPRIORITY);
        ret = hvdxg_send_sync_vgpu(&get, sizeof(get), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.schedprio_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.priority = result.priority;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        hvdxg.schedprio_last_len = actual_len;
        hvdxg.schedprio_last_ret = ret;
        hvdxg.schedprio_last_context = req.context.v;
        hvdxg.schedprio_last_value = req.priority;
        break;
    }

    case LX_DXSETALLOCATIONPRIORITY: {
        struct d3dkmt_setallocationpriority req;
        uint8 command_buf[sizeof(struct hvdxg_command_setallocationpriority) -
                          1 + HV_DXG_ALLOCATION_MAX *
                                  sizeof(struct hvdxg_d3dkmthandle) +
                          HV_DXG_ALLOCATION_MAX * sizeof(uint32)];
        struct hvdxg_command_setallocationpriority *set =
            (struct hvdxg_command_setallocationpriority *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 alloc_size = 0;
        uint32 priority_count;
        uint32 priority_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.priorities == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX) {
            ret = -EINVAL;
            break;
        }
        if (req.resource.v != 0) {
            if (req.allocation_count != 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = 1;
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        } else {
            if (req.allocation_count == 0 || req.allocation_list == 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = req.allocation_count;
            alloc_size = req.allocation_count *
                         sizeof(struct hvdxg_d3dkmthandle);
        }
        priority_size = priority_count * sizeof(uint32);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &set->hdr, HV_DXGK_VMBCOMMAND_SETALLOCATIONPRIORITY,
            hvdxg.dxg_process);
        set->device.v = req.device.v;
        set->resource.v = req.resource.v;
        set->allocation_count = req.allocation_count;
        if (alloc_size != 0 &&
            either_copyin(set->data, 1, req.allocation_list,
                          alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (alloc_size != 0) {
            struct hvdxg_d3dkmthandle *allocations =
                (struct hvdxg_d3dkmthandle *)set->data;
            if (!hvdxg_owner_has_device(owner, req.device.v)) {
                ret = -EPERM;
                break;
            }
            for (uint32 i = 0; i < req.allocation_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                                allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        if (either_copyin(set->data + alloc_size, 1, req.priorities,
                          priority_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = hvdxg_send_sync_vgpu(set,
                                   sizeof(*set) - 1 + alloc_size +
                                       priority_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.allocprio_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.allocprio_last_len = actual_len;
        hvdxg.allocprio_last_ret = ret;
        hvdxg.allocprio_last_count = req.allocation_count;
        break;
    }

    case LX_DXGETALLOCATIONPRIORITY: {
        struct d3dkmt_getallocationpriority req;
        uint8 command_buf[sizeof(struct hvdxg_command_getallocationpriority) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        uint8 result_buf[sizeof(struct hvdxg_command_getallocationpriority_return) +
                         (HV_DXG_ALLOCATION_MAX - 1) * sizeof(uint32)];
        struct hvdxg_command_getallocationpriority *get =
            (struct hvdxg_command_getallocationpriority *)command_buf;
        struct hvdxg_command_getallocationpriority_return *result =
            (struct hvdxg_command_getallocationpriority_return *)result_buf;
        uint32 actual_len = 0;
        uint32 alloc_size = 0;
        uint32 priority_count;
        uint32 priority_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.priorities == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX) {
            ret = -EINVAL;
            break;
        }
        if (req.resource.v != 0) {
            if (req.allocation_count != 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = 1;
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        } else {
            if (req.allocation_count == 0 || req.allocation_list == 0) {
                ret = -EINVAL;
                break;
            }
            priority_count = req.allocation_count;
            alloc_size = req.allocation_count *
                         sizeof(struct hvdxg_d3dkmthandle);
        }
        priority_size = priority_count * sizeof(uint32);
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &get->hdr, HV_DXGK_VMBCOMMAND_GETALLOCATIONPRIORITY,
            hvdxg.dxg_process);
        get->device.v = req.device.v;
        get->resource.v = req.resource.v;
        get->allocation_count = req.allocation_count;
        if (alloc_size != 0 &&
            either_copyin(get->allocations, 1, req.allocation_list,
                          alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (alloc_size != 0) {
            if (!hvdxg_owner_has_device(owner, req.device.v)) {
                ret = -EPERM;
                break;
            }
            for (uint32 i = 0; i < req.allocation_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                                get->allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        ret = hvdxg_send_sync_vgpu(get, sizeof(*get) - sizeof(get->allocations) +
                                        alloc_size,
                                   result, sizeof(result_buf), &actual_len);
        if (actual_len >= sizeof(result->status))
            hvdxg.allocprio_last_status = result->status.v;
        if (ret == 0 && actual_len >= sizeof(result->status))
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret == 0 && actual_len >= sizeof(result->status) + priority_size) {
            ret = either_copyout(1, req.priorities, result->priorities,
                                 priority_size) < 0 ? -EFAULT : 0;
        }
        hvdxg.allocprio_last_len = actual_len;
        hvdxg.allocprio_last_ret = ret;
        hvdxg.allocprio_last_count = req.allocation_count;
        break;
    }

    case LX_DXQUERYALLOCATIONRESIDENCY: {
        struct d3dkmt_queryallocationresidency req;
        uint8 command_buf[sizeof(struct hvdxg_command_queryallocationresidency) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        uint8 result_buf[sizeof(struct hvdxg_command_queryallocationresidency_return) +
                         (HV_DXG_ALLOCATION_MAX - 1) *
                             sizeof(enum d3dkmt_allocationresidencystatus)];
        struct hvdxg_command_queryallocationresidency *query =
            (struct hvdxg_command_queryallocationresidency *)command_buf;
        struct hvdxg_command_queryallocationresidency_return *result =
            (struct hvdxg_command_queryallocationresidency_return *)result_buf;
        uint32 actual_len = 0;
        uint32 alloc_count;
        uint32 alloc_size = 0;
        uint32 result_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.residency_status == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX) {
            ret = -EINVAL;
            break;
        }
        if (req.allocation_count != 0) {
            if (req.allocations == 0) {
                ret = -EINVAL;
                break;
            }
            alloc_count = req.allocation_count;
            alloc_size = alloc_count * sizeof(struct hvdxg_d3dkmthandle);
        } else {
            if (req.resource.v == 0) {
                ret = -EINVAL;
                break;
            }
            alloc_count = 1;
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            req.resource.v, 0)) {
                ret = -EPERM;
                break;
            }
        }
        result_size = sizeof(result->status) +
                      alloc_count *
                          sizeof(enum d3dkmt_allocationresidencystatus);
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &query->hdr, HV_DXGK_VMBCOMMAND_QUERYALLOCATIONRESIDENCY,
            hvdxg.dxg_process);
        query->args = req;
        if (alloc_size != 0 &&
            either_copyin(query->allocations, 1, req.allocations,
                          alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (alloc_size != 0) {
            if (!hvdxg_owner_has_device(owner, req.device.v)) {
                ret = -EPERM;
                break;
            }
            for (uint32 i = 0; i < req.allocation_count; i++) {
                if (!hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                                query->allocations[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        ret = hvdxg_send_sync_vgpu(query,
                                   sizeof(*query) -
                                       sizeof(query->allocations) +
                                       alloc_size,
                                   result, result_size, &actual_len);
        if (actual_len >= sizeof(result->status))
            hvdxg.allocres_last_status = result->status.v;
        if (ret == 0 && actual_len >= sizeof(result->status))
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret == 0 && actual_len >= result_size) {
            ret = either_copyout(1, req.residency_status,
                                 result->residency_status,
                                 result_size - sizeof(result->status)) < 0 ?
                  -EFAULT : 0;
            hvdxg.allocres_last_value = result->residency_status[0];
        }
        hvdxg.allocres_last_len = actual_len;
        hvdxg.allocres_last_ret = ret;
        hvdxg.allocres_last_count = alloc_count;
        break;
    }

    case LX_DXOFFERALLOCATIONS: {
        struct d3dkmt_offerallocations req;
        uint8 command_buf[sizeof(struct hvdxg_command_offerallocations) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_offerallocations *offer =
            (struct hvdxg_command_offerallocations *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 alloc_size;
        uint64 list_ptr;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.offer_last_status = 0;
        hvdxg.offer_last_count = req.allocation_count;
        if (req.device.v == 0 || req.allocation_count == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX ||
            ((req.resources == 0) == (req.allocations == 0))) {
            ret = -EINVAL;
            hvdxg.offer_last_ret = ret;
            break;
        }
        alloc_size = req.allocation_count *
                     sizeof(struct hvdxg_d3dkmthandle);
        list_ptr = req.resources != 0 ? req.resources : req.allocations;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &offer->hdr, HV_DXGK_VMBCOMMAND_OFFERALLOCATIONS,
            hvdxg.dxg_process);
        offer->device.v = req.device.v;
        offer->allocation_count = req.allocation_count;
        offer->priority = req.priority;
        offer->flags = req.flags;
        offer->resources = req.resources != 0 ? 1 : 0;
        if (either_copyin(offer->allocations, 1, list_ptr, alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.allocation_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, req.device.v,
                                            offer->resources ?
                                            offer->allocations[i].v : 0,
                                            offer->resources ? 0 :
                                            offer->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        ret = hvdxg_send_sync_vgpu(offer,
                                   sizeof(*offer) -
                                       sizeof(offer->allocations) +
                                       alloc_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.offer_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.offer_last_len = actual_len;
        hvdxg.offer_last_ret = ret;
        break;
    }

    case LX_DXRECLAIMALLOCATIONS2: {
        struct d3dkmt_reclaimallocations2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_reclaimallocations) +
                          (HV_DXG_ALLOCATION_MAX - 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        uint8 result_buf[sizeof(struct hvdxg_command_reclaimallocations_return) +
                         (HV_DXG_ALLOCATION_MAX - 1) *
                             sizeof(enum d3dddi_reclaim_result)];
        struct hvdxg_command_reclaimallocations *reclaim =
            (struct hvdxg_command_reclaimallocations *)command_buf;
        struct hvdxg_command_reclaimallocations_return *result =
            (struct hvdxg_command_reclaimallocations_return *)result_buf;
        uint32 actual_len = 0;
        uint32 alloc_size;
        uint32 result_size;
        uint64 list_ptr;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.reclaim_last_status = 0;
        hvdxg.reclaim_last_count = req.allocation_count;
        hvdxg.reclaim_last_result0 = 0;
        hvdxg.reclaim_last_fence = 0;
        if (req.paging_queue.v == 0 || req.allocation_count == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX ||
            ((req.resources == 0) == (req.allocations == 0))) {
            ret = -EINVAL;
            hvdxg.reclaim_last_ret = ret;
            break;
        }
        alloc_size = req.allocation_count *
                     sizeof(struct hvdxg_d3dkmthandle);
        result_size = sizeof(*result);
        if (req.results != 0)
            result_size += (req.allocation_count - 1) *
                           sizeof(enum d3dddi_reclaim_result);
        list_ptr = req.resources != 0 ? req.resources : req.allocations;
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &reclaim->hdr, HV_DXGK_VMBCOMMAND_RECLAIMALLOCATIONS,
            hvdxg.dxg_process);
        reclaim->paging_queue.v = req.paging_queue.v;
        reclaim->allocation_count = req.allocation_count;
        reclaim->resources = req.resources != 0 ? 1 : 0;
        reclaim->write_results = req.results != 0 ? 1 : 0;
        if (either_copyin(reclaim->allocations, 1, list_ptr, alloc_size) < 0) {
            ret = -EFAULT;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.allocation_count; i++) {
            if (!hvdxg_owner_has_allocation(owner, 0,
                                            reclaim->resources ?
                                            reclaim->allocations[i].v : 0,
                                            reclaim->resources ? 0 :
                                            reclaim->allocations[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        reclaim->device.v = reclaim->resources ?
                            hvdxg.last_device_handle :
                            hvdxg_device_for_allocation(
                                reclaim->allocations[0].v);
        if (reclaim->device.v == 0) {
            ret = -EINVAL;
            hvdxg.reclaim_last_ret = ret;
            break;
        }
        ret = hvdxg_send_sync_vgpu(reclaim,
                                   sizeof(*reclaim) -
                                       sizeof(reclaim->allocations) +
                                       alloc_size,
                                   result, result_size, &actual_len);
        if (actual_len >= sizeof(result->paging_fence_value) +
                          sizeof(result->status)) {
            hvdxg.reclaim_last_fence = result->paging_fence_value;
            hvdxg.reclaim_last_status = result->status.v;
        }
        if (ret == 0 &&
            actual_len >= sizeof(result->paging_fence_value) +
                          sizeof(result->status))
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret == 0) {
            req.paging_fence_value = result->paging_fence_value;
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0) {
                ret = -EFAULT;
                break;
            }
            if (req.results != 0 && actual_len >= result_size) {
                hvdxg.reclaim_last_result0 = result->results[0];
                ret = either_copyout(1, req.results, result->results,
                                     req.allocation_count *
                                         sizeof(enum d3dddi_reclaim_result)) < 0 ?
                      -EFAULT : 0;
            }
        }
        hvdxg.reclaim_last_len = actual_len;
        hvdxg.reclaim_last_ret = ret;
        break;
    }

    case LX_DXUPDATEGPUVIRTUALADDRESS: {
        struct d3dkmt_updategpuvirtualaddress req;
        struct hvdxg_command_updategpuvirtualaddress *update = NULL;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 op_size;
        uint32 command_len;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.num_operations == 0 ||
            req.operations == 0 ||
            req.num_operations >
                HV_DXG_VM_BUS_PACKET_MAX /
                    sizeof(struct d3dddi_updategpuvirtualaddress_operation)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            (req.context.v != 0 &&
             !hvdxg_owner_has_context(owner, req.context.v)) ||
            (req.fence_object.v != 0 &&
             !hvdxg_owner_has_sync(owner, req.fence_object.v))) {
            ret = -EPERM;
            break;
        }
        op_size = req.num_operations *
                  sizeof(struct d3dddi_updategpuvirtualaddress_operation);
        command_len = sizeof(*update) - sizeof(update->operations) + op_size;
        if (command_len > HV_DXG_VM_BUS_PACKET_MAX) {
            ret = -EINVAL;
            break;
        }
        update = kvmalloc(command_len);
        if (update == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(update, 0, command_len);
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &update->hdr, HV_DXGK_VMBCOMMAND_UPDATEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        update->fence_value = req.fence_value;
        update->device.v = req.device.v;
        update->context.v = req.context.v;
        update->fence_object.v = req.fence_object.v;
        update->num_operations = req.num_operations;
        update->flags = req.flags.value;
        if (either_copyin(update->operations, 1, req.operations,
                          op_size) < 0) {
            ret = -EFAULT;
            kvfree(update);
            update = NULL;
            break;
        }
        ret = hvdxg_send_sync_vgpu(update, command_len, &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.updategpuva_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.updategpuva_last_len = actual_len;
        hvdxg.updategpuva_last_ret = ret;
        hvdxg.updategpuva_last_ops = req.num_operations;
        hvdxg.updategpuva_last_fence = req.fence_value;
        if (update != NULL)
            kvfree(update);
        break;
    }

    case LX_DXLOCK2: {
        struct d3dkmt_lock2 req;
        struct hvdxg_command_lock2 lock;
        struct hvdxg_command_lock2_return result;
        struct hvdxg_tracked_allocation *tracked;
        uint32 actual_len = 0;
        uint64 map_size = 0;
        uint64 mapped_size;
        uint64 map_pa;
        uint64 map_extra_flags;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.lock2_last_allocation = req.allocation.v;
        hvdxg.lock2_last_status = 0;
        hvdxg.lock2_last_offset = 0;
        hvdxg.lock2_last_user_va = 0;
        if (req.device.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                        req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        tracked = hvdxg_owner_find_allocation(owner, req.device.v, 0,
                                              req.allocation.v);
        if (tracked != NULL && tracked->cpu_va != 0 &&
            tracked->cpu_vm != NULL &&
            (current == NULL || current->vm == NULL ||
             tracked->cpu_vm != current->vm)) {
            ret = -EBUSY;
            break;
        }
        memset(&lock, 0, sizeof(lock));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&lock.hdr,
                                        HV_DXGK_VMBCOMMAND_LOCK2,
                                        hvdxg.dxg_process);
        lock.args = req;
        lock.args.data = 0;
        ret = hvdxg_send_sync_vgpu(&lock, sizeof(lock), &result,
                                   sizeof(result), &actual_len);
        hvdxg.lock2_last_len = actual_len;
        hvdxg.lock2_last_ret = ret;
        if (actual_len >= sizeof(result.status))
            hvdxg.lock2_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0) {
            hvdxg_note_lock2_history(actual_len, ret,
                                     (uint32)hvdxg.lock2_last_status,
                                     req.device.v, req.allocation.v, 0, 0, 0);
            break;
        }
        if (actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
            hvdxg.lock2_last_ret = ret;
            hvdxg_note_lock2_history(actual_len, ret,
                                     (uint32)hvdxg.lock2_last_status,
                                     req.device.v, req.allocation.v, 0, 0, 0);
            break;
        }
        hvdxg.lock2_last_offset = result.cpu_visible_buffer_offset;
        map_size = hvdxg_owner_allocation_size(owner, req.device.v, 0,
                                               req.allocation.v);
        if (map_size == 0 && hvdxg.last_allocation_handle == req.allocation.v)
            map_size = hvdxg.last_allocation_size;
        if (map_size == 0)
            map_size = PGSIZE;
        map_pa = result.cpu_visible_buffer_offset;
        if (tracked != NULL && tracked->cpu_va != 0) {
            req.data = tracked->cpu_va;
            tracked->lock_refcount++;
        } else {
            map_extra_flags =
                hv_cmdline_enabled("dxg_lock_cached") ||
                (tracked != NULL &&
                 (tracked->flags & HV_DXG_ALLOCATION_FLAG_CACHED)) ?
                0 : HV_DXG_PTE_WRITE_THROUGH;
            req.data = hvdxg_map_iospace_user(map_pa, map_size,
                                              map_extra_flags);
            if (req.data == 0 && hvdxg.iospace_set &&
                result.cpu_visible_buffer_offset < hvdxg.iospace_size) {
                map_pa = hvdxg.iospace_base + result.cpu_visible_buffer_offset;
                req.data = hvdxg_map_iospace_user(map_pa, map_size,
                                                  map_extra_flags);
            }
            if (tracked != NULL && req.data != 0) {
                mapped_size = PGROUNDUP((map_pa & (PGSIZE - 1)) +
                                        map_size);
                tracked->cpu_va = req.data;
                tracked->map_size = mapped_size;
                tracked->cpu_vm = current ? current->vm : NULL;
                tracked->lock_refcount = 1;
            }
        }
        hvdxg.lock2_last_user_va = req.data;
        if (req.data == 0) {
            ret = -ENOMEM;
            hvdxg.lock2_last_ret = ret;
            hvdxg_note_lock2_history(actual_len, ret,
                                     (uint32)hvdxg.lock2_last_status,
                                     req.device.v, req.allocation.v,
                                     hvdxg.lock2_last_offset, 0, map_size);
            break;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.lock2_last_ret = ret;
        hvdxg_note_lock2_history(actual_len, ret,
                                 (uint32)hvdxg.lock2_last_status,
                                 req.device.v, req.allocation.v,
                                 hvdxg.lock2_last_offset,
                                 hvdxg.lock2_last_user_va, map_size);
        break;
    }

    case LX_DXUNLOCK2: {
        struct d3dkmt_unlock2 req;
        struct hvdxg_command_unlock2 unlock;
        struct hvdxg_ntstatus status;
        struct hvdxg_tracked_allocation *tracked;
        uint32 actual_len = 0;
        int skip_host_unlock = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.unlock2_last_allocation = req.allocation.v;
        hvdxg.unlock2_last_status = 0;
        if (req.device.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v) ||
            !hvdxg_owner_has_allocation(owner, req.device.v, 0,
                                        req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        tracked = hvdxg_owner_find_allocation(owner, req.device.v, 0,
                                              req.allocation.v);
        if (tracked == NULL || tracked->cpu_va == 0) {
            ret = -EINVAL;
            break;
        }
        if (tracked->lock_refcount > 0) {
            tracked->lock_refcount--;
            if (tracked->lock_refcount != 0)
                skip_host_unlock = 1;
            else
                (void)hvdxg_unmap_tracked_allocation(tracked);
        } else {
            skip_host_unlock = 1;
        }
        if (skip_host_unlock) {
            hvdxg.unlock2_last_len = 0;
            hvdxg.unlock2_last_ret = 0;
            break;
        }
        memset(&unlock, 0, sizeof(unlock));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&unlock.hdr,
                                        HV_DXGK_VMBCOMMAND_UNLOCK2,
                                        hvdxg.dxg_process);
        unlock.args = req;
        ret = hvdxg_send_sync_vgpu(&unlock, sizeof(unlock), &status,
                                   sizeof(status), &actual_len);
        hvdxg.unlock2_last_len = actual_len;
        hvdxg.unlock2_last_ret = ret;
        if (actual_len >= sizeof(status))
            hvdxg.unlock2_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.unlock2_last_ret = ret;
        break;
    }

    case LX_DXCREATESYNCHRONIZATIONOBJECT: {
        struct d3dkmt_createsynchronizationobject2 req;
        struct hvdxg_command_createsyncobject create;
        struct hvdxg_command_createsyncobject_return result;
        uint32 actual_len = 0;
        uint64 fence_kva = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        memset(&create, 0, sizeof(create));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&create.hdr,
                                        HV_DXGK_VMBCOMMAND_CREATESYNCOBJECT,
                                        hvdxg.dxg_process);
        create.args = req;
        create.args.sync_object.v = 0;
        create.client_hint = 1;
        ret = hvdxg_send_sync_vgpu(&create, sizeof(create), &result,
                                   sizeof(result), &actual_len);
        hvdxg.syncobject_last_len = actual_len;
        hvdxg.syncobject_last_ret = ret;
        hvdxg.syncobject_last_handle = result.sync_object.v;
        hvdxg.syncobject_last_fence_cpu = 0;
        hvdxg.syncobject_last_fence_gpu = result.fence_gpu_va;
        hvdxg.syncobject_last_fence_pa = result.fence_storage_address;
        hvdxg.syncobject_last_fence_off = result.fence_storage_offset;
        if (ret != 0)
            break;
        if (actual_len < sizeof(result) || result.sync_object.v == 0) {
            ret = -EIO;
            break;
        }
        req.sync_object.v = result.sync_object.v;
        if (req.info.flags.shared)
            req.info.shared_handle.v = result.global_sync_object.v;
        if (req.info.type == _D3DDDI_MONITORED_FENCE) {
            req.info.monitored_fence.fence_cpu_virtual_address =
                hvdxg_map_iospace_user(result.fence_storage_address,
                                       PGSIZE, 0);
            fence_kva = hvdxg_map_iospace_kernel(result.fence_storage_address,
                                                 PGSIZE);
            hvdxg.syncobject_last_fence_cpu =
                req.info.monitored_fence.fence_cpu_virtual_address;
            if (req.info.monitored_fence.fence_cpu_virtual_address == 0) {
                ret = -ENOMEM;
                break;
            }
            req.info.monitored_fence.fence_gpu_virtual_address =
                result.fence_gpu_va;
        } else if (req.info.type == _D3DDDI_PERIODIC_MONITORED_FENCE) {
            req.info.periodic_monitored_fence.fence_cpu_virtual_address =
                hvdxg_map_iospace_user(result.fence_storage_address,
                                       PGSIZE, 0);
            fence_kva = hvdxg_map_iospace_kernel(result.fence_storage_address,
                                                 PGSIZE);
            hvdxg.syncobject_last_fence_cpu =
                req.info.periodic_monitored_fence.fence_cpu_virtual_address;
            if (req.info.periodic_monitored_fence.fence_cpu_virtual_address ==
                0) {
                ret = -ENOMEM;
                break;
            }
            req.info.periodic_monitored_fence.fence_gpu_virtual_address =
                result.fence_gpu_va;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL)
            hvdxg_track_sync(owner, req.device.v, req.sync_object.v,
                             req.info.type, req.info.flags.value,
                             result.global_sync_object.v,
                             hvdxg.syncobject_last_fence_cpu, fence_kva);
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECT: {
        struct d3dkmt_signalsynchronizationobject2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              sizeof(struct hvdxg_d3dkmthandle) +
                          (D3DDDI_MAX_BROADCAST_CONTEXT + 1) *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 context_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.flags.enqueue_cpu_event) {
            hvdxg_note_unsupported_ioctl(
                cmd, 0, (uint32)req.cpu_event_handle, req.object_count);
            ret = -ENOTSUP;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.context_count >= D3DDDI_MAX_BROADCAST_CONTEXT) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, req.object_array[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        for (uint32 i = 0; ret == 0 && i < req.context_count; i++) {
            if (!hvdxg_owner_has_context(owner, req.contexts[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        context_size = req.context_count *
                       sizeof(struct hvdxg_d3dkmthandle);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = req.context_count + 1;
        signal->fence_value = req.fence.fence_value;
        signal->u.device.v = 0;
        pos = (uint8 *)&signal[1];
        memcpy(pos, req.object_array, object_size);
        pos += object_size;
        memcpy(pos, &req.context, sizeof(req.context));
        pos += sizeof(req.context);
        memcpy(pos, req.contexts, context_size);
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       sizeof(req.context) + context_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXWAITFORSYNCHRONIZATIONOBJECT: {
        struct d3dkmt_waitforsynchronizationobject2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          D3DDDI_MAX_OBJECT_WAITED_ON *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_waitsyncobjectfromgpu *wait =
            (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        struct hvdxg_ntstatus status;
        uint64 fence_values[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, req.object_array[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        for (uint32 i = 0; i < req.object_count; i++)
            fence_values[i] = req.fence.fence_value;
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_wait_last_status = 0;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.context.v;
        wait->object_count = req.object_count;
        wait->legacy_fence_object = 1;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, fence_values, fence_size);
        pos += fence_size;
        memcpy(pos, req.object_array, object_size);
        ret = hvdxg_send_sync_vgpu(wait,
                                   sizeof(*wait) - sizeof(uint64) +
                                       fence_size + object_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_wait_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_wait_last_len = actual_len;
        hvdxg.syncgpu_wait_last_ret = ret;
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU: {
        struct d3dkmt_signalsynchronizationobjectfromcpu req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.objects == 0 || req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncsignal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = 0;
        signal->fence_value = 0;
        signal->u.device.v = req.device.v;
        pos = (uint8 *)&signal[1];
        if (either_copyin(pos, 1, req.objects, object_size) < 0 ||
            either_copyin(pos + object_size, 1, req.fence_values,
                          fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *objects =
                (struct hvdxg_d3dkmthandle *)(uint8 *)&signal[1];
            for (uint32 i = 0; i < req.object_count; i++) {
                if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncsignal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncsignal_last_len = actual_len;
        hvdxg.syncsignal_last_ret = ret;
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU: {
        struct d3dkmt_signalsynchronizationobjectfromgpu req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64)) +
                          sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.objects == 0 || req.monitored_fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->context_count = 1;
        pos = (uint8 *)&signal[1];
        if (either_copyin(pos, 1, req.objects, object_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *objects =
                (struct hvdxg_d3dkmthandle *)pos;
            for (uint32 i = 0; i < req.object_count; i++) {
                if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        pos += object_size;
        memcpy(pos, &req.context, sizeof(req.context));
        pos += sizeof(req.context);
        if (either_copyin(pos, 1, req.monitored_fence_values,
                          fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       sizeof(req.context) + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU2: {
        struct d3dkmt_signalsynchronizationobjectfromgpu2 req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64)) +
                          D3DDDI_MAX_BROADCAST_CONTEXT *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 context_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.flags.enqueue_cpu_event) {
            hvdxg_note_unsupported_ioctl(
                cmd, 0, (uint32)req.cpu_event_handle, req.object_count);
            ret = -ENOTSUP;
            break;
        }
        if (req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.context_count == 0 ||
            req.context_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
            req.objects == 0 || req.contexts == 0 ||
            req.monitored_fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        context_size = req.context_count *
                       sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = req.context_count;
        signal->fence_value = 0;
        signal->u.device.v = 0;
        pos = (uint8 *)&signal[1];
        if (either_copyin(pos, 1, req.objects, object_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *objects =
                (struct hvdxg_d3dkmthandle *)pos;
            for (uint32 i = 0; i < req.object_count; i++) {
                if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        pos += object_size;
        if (either_copyin(pos, 1, req.contexts, context_size) < 0) {
            ret = -EFAULT;
            break;
        }
        {
            struct hvdxg_d3dkmthandle *contexts =
                (struct hvdxg_d3dkmthandle *)pos;
            for (uint32 i = 0; i < req.context_count; i++) {
                if (!hvdxg_owner_has_context(owner, contexts[i].v)) {
                    ret = -EPERM;
                    break;
                }
            }
            if (ret != 0)
                break;
        }
        pos += context_size;
        if (either_copyin(pos, 1, req.monitored_fence_values,
                          fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       context_size + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMCPU: {
        struct d3dkmt_waitforsynchronizationobjectfromcpu req;
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 fence_values[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 event_id = 0;
        struct vfs_file *async_event_file = NULL;
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
            req.objects == 0 || req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(objects, 0, sizeof(objects));
        memset(fence_values, 0, sizeof(fence_values));
        if (either_copyin(objects, 1, req.objects, object_size) < 0 ||
            either_copyin(fence_values, 1, req.fence_values, fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        if (hvdxg_wait_cpu_fences_already_satisfied(
                owner, objects, fence_values, req.object_count,
                req.flags.wait_any)) {
            hvdxg.syncwait_last_len = 0;
            hvdxg.syncwait_last_ret = 0;
            hvdxg.syncwait_last_status = 0;
            ret = 0;
            break;
        }
        if (req.async_event != 0) {
            async_event_file =
                vfs_fdtable_get_file(current->fdtable, (int)req.async_event);
            if (async_event_file == NULL) {
                ret = -EINVAL;
                break;
            }
            if (!eventfd_file_is_eventfd(async_event_file)) {
                vfs_fput(async_event_file);
                ret = -EINVAL;
                break;
            }
            event_id = hvdxg_alloc_host_event_file(async_event_file, 1);
        } else {
            event_id = hvdxg_alloc_host_event();
        }
        if (event_id == 0) {
            if (async_event_file != NULL)
                vfs_fput(async_event_file);
            ret = -ENOMEM;
            break;
        }
        ret = hvdxg_send_waitsyncobjectfromcpu(&req, objects, fence_values,
                                               event_id, object_size,
                                               fence_size, &actual_len);
        if (ret == 0 && req.async_event != 0)
            hvdxg_pump_events_ms(20);
        if (ret == 0 && req.async_event == 0) {
            ret = hvdxg_wait_host_event(event_id,
                                        HV_DXG_HOST_EVENT_TIMEOUT_MS);
            hvdxg.syncwait_last_ret = ret;
        }
        if (ret != 0 || req.async_event == 0)
            hvdxg_remove_host_event(event_id);
        break;
    }

    case LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMGPU: {
        struct d3dkmt_waitforsynchronizationobjectfromgpu req;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          D3DDDI_MAX_OBJECT_WAITED_ON *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_waitsyncobjectfromgpu *wait =
            (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 fences[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        int monitored_fence = 0;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
            req.objects == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        memset(objects, 0, sizeof(objects));
        memset(fences, 0, sizeof(fences));
        if (either_copyin(objects, 1, req.objects, object_size) < 0) {
            ret = -EFAULT;
            break;
        }
        monitored_fence =
            hvdxg_owner_sync_is_monitored(owner, objects[0].v);
        if (monitored_fence) {
            if (req.monitored_fence_values == 0) {
                ret = -EINVAL;
                break;
            }
            if (either_copyin(fences, 1, req.monitored_fence_values,
                              fence_size) < 0) {
                ret = -EFAULT;
                break;
            }
        } else {
            if (req.object_count != 1) {
                ret = -EINVAL;
                break;
            }
            fences[0] = req.fence_value;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_wait_last_status = 0;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.context.v;
        wait->object_count = req.object_count;
        wait->legacy_fence_object = monitored_fence ? 0 : 1;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, fences, fence_size);
        pos += fence_size;
        memcpy(pos, objects, object_size);
        hvdxg.syncgpu_wait_last_context = req.context.v;
        hvdxg.syncgpu_wait_last_object = objects[0].v;
        hvdxg.syncgpu_wait_last_object_count = req.object_count;
        hvdxg.syncgpu_wait_last_object_type =
            hvdxg_owner_sync_type(owner, objects[0].v);
        hvdxg.syncgpu_wait_last_legacy = wait->legacy_fence_object;
        hvdxg.syncgpu_wait_last_fence = fences[0];
        hvdxg.syncgpu_wait_last_cmd_len =
            sizeof(*wait) - sizeof(uint64) + fence_size + object_size;
        ret = hvdxg_send_sync_vgpu(wait,
                                   sizeof(*wait) - sizeof(uint64) +
                                       fence_size + object_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_wait_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_wait_last_len = actual_len;
        hvdxg.syncgpu_wait_last_ret = ret;
        break;
    }

    case LX_DXDESTROYSYNCHRONIZATIONOBJECT: {
        struct d3dkmt_destroysynchronizationobject req;
        struct hvdxg_command_destroysyncobject destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.sync_object.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_sync(owner, req.sync_object.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vm_init(&destroy.hdr,
                              HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT);
        destroy.hdr.process = hvdxg.dxg_process;
        destroy.sync_object.v = req.sync_object.v;
        ret = hvdxg_send_sync_global(&destroy, sizeof(destroy), &status,
                                     sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        if (ret == 0 && owner != NULL)
            hvdxg_untrack_sync(owner, req.sync_object.v);
        break;
    }

    case LX_DXCREATECONTEXTVIRTUAL: {
        struct d3dkmt_createcontextvirtual req;
        uint8 *command_buf = NULL;
        struct hvdxg_command_createcontextvirtual *create;
        uint32 command_len;
        uint32 actual_len = 0;
        uint32 private_max = HV_DXG_VM_BUS_PACKET_MAX -
                             sizeof(struct hvdxg_command_createcontextvirtual) +
                             1;
        struct hvdxg_ntstatus short_status;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 ||
            req.priv_drv_data_size > private_max ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EPERM;
            break;
        }
        command_len = sizeof(struct hvdxg_command_createcontextvirtual);
        if (req.priv_drv_data_size != 0)
            command_len += req.priv_drv_data_size - 1;
        command_buf = kvmalloc(command_len);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(command_buf, 0, command_len);
        create = (struct hvdxg_command_createcontextvirtual *)command_buf;
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL,
                                        hvdxg.dxg_process);
        create->device.v = req.device.v;
        create->node_ordinal = req.node_ordinal;
        create->engine_affinity = req.engine_affinity;
        create->flags = req.flags;
        create->client_hint = (uint32)req.client_hint;
        create->priv_drv_data_size = req.priv_drv_data_size;
        hvdxg.createcontext_last_device = req.device.v;
        hvdxg.createcontext_last_node = req.node_ordinal;
        hvdxg.createcontext_last_engine = req.engine_affinity;
        hvdxg.createcontext_last_flags = req.flags.value;
        hvdxg.createcontext_last_hint = (uint32)req.client_hint;
        hvdxg.createcontext_last_priv_size = req.priv_drv_data_size;
        hvdxg.createcontext_last_priv_head_len = 0;
        memset(hvdxg.createcontext_last_priv_head, 0,
               sizeof(hvdxg.createcontext_last_priv_head));
        if (req.priv_drv_data_size != 0 &&
            either_copyin(create->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createcontext_done;
        }
        if (req.priv_drv_data_size != 0)
            hvdxg_save_priv_head(hvdxg.createcontext_last_priv_head,
                                 sizeof(hvdxg.createcontext_last_priv_head),
                                 &hvdxg.createcontext_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        ret = hvdxg_send_sync_vgpu(create, command_len, command_buf,
                                   command_len, &actual_len);
        hvdxg.createcontext_last_len = actual_len;
        hvdxg.createcontext_last_ret = ret;
        hvdxg.createcontext_last_handle = create->context.v;
        if (ret != 0) {
            hvdxg.createcontext_fail_len = actual_len;
            hvdxg.createcontext_fail_ret = ret;
            hvdxg.createcontext_fail_status = 0;
            goto createcontext_done;
        }
        if (actual_len < sizeof(*create) - 1 || create->context.v == 0) {
            memset(&short_status, 0, sizeof(short_status));
            if (actual_len >= sizeof(short_status)) {
                memcpy(&short_status, command_buf, sizeof(short_status));
                ret = hvdxg_ntstatus_to_errno(short_status);
                if (ret >= 0)
                    ret = -EIO;
            } else {
                ret = -EIO;
            }
            hvdxg.createcontext_fail_len = actual_len;
            hvdxg.createcontext_fail_ret = ret;
            hvdxg.createcontext_fail_status = short_status.v;
            hvdxg.createcontext_last_ret = ret;
            goto createcontext_done;
        }
        req.context.v = create->context.v;
        hvdxg.createcontext_last_handle = req.context.v;
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1)
            hvdxg_save_priv_head(hvdxg.createcontext_last_priv_head,
                                 sizeof(hvdxg.createcontext_last_priv_head),
                                 &hvdxg.createcontext_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1 &&
            either_copyout(1, req.priv_drv_data, create->priv_drv_data,
                           req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createcontext_done;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            if (hvdxg_track_u32_grow(&owner->contexts,
                                      &owner->context_count,
                                      &owner->context_capacity,
                                      req.context.v) != 0) {
                (void)hvdxg_destroy_context_host(req.context.v);
                ret = -ENOMEM;
            }
        }
createcontext_done:
        if (command_buf)
            kvfree(command_buf);
        break;
    }

    case LX_DXCREATEHWQUEUE: {
        struct d3dkmt_createhwqueue req;
        uint8 *command_buf = NULL;
        struct hvdxg_command_createhwqueue *create;
        uint32 command_len;
        uint32 actual_len = 0;
        uint32 private_max = HV_DXG_VM_BUS_PACKET_MAX -
                             sizeof(struct hvdxg_command_createhwqueue) + 1;
        uint64 fence_kva = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0 ||
            req.priv_drv_data_size > private_max ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        command_len = sizeof(struct hvdxg_command_createhwqueue);
        if (req.priv_drv_data_size != 0)
            command_len += req.priv_drv_data_size - 1;
        command_buf = kvmalloc(command_len);
        if (command_buf == NULL) {
            ret = -ENOMEM;
            break;
        }
        memset(command_buf, 0, command_len);
        create = (struct hvdxg_command_createhwqueue *)command_buf;
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATEHWQUEUE,
                                        hvdxg.dxg_process);
        create->context.v = req.context.v;
        create->flags = req.flags;
        create->priv_drv_data_size = req.priv_drv_data_size;
        hvdxg.createhwqueue_last_context = req.context.v;
        hvdxg.createhwqueue_last_flags = req.flags.value;
        hvdxg.createhwqueue_last_priv_size = req.priv_drv_data_size;
        hvdxg.createhwqueue_last_priv_head_len = 0;
        memset(hvdxg.createhwqueue_last_priv_head, 0,
               sizeof(hvdxg.createhwqueue_last_priv_head));
        if (req.priv_drv_data_size != 0 &&
            either_copyin(create->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createhwqueue_done;
        }
        if (req.priv_drv_data_size != 0)
            hvdxg_save_priv_head(hvdxg.createhwqueue_last_priv_head,
                                 sizeof(hvdxg.createhwqueue_last_priv_head),
                                 &hvdxg.createhwqueue_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        ret = hvdxg_send_sync_vgpu(create, command_len, command_buf,
                                   command_len, &actual_len);
        hvdxg.createhwqueue_last_len = actual_len;
        hvdxg.createhwqueue_last_ret = ret;
        hvdxg.createhwqueue_last_status =
            actual_len >= sizeof(create->status) ? create->status.v : 0;
        if (ret == 0 && actual_len >= sizeof(create->status))
            ret = hvdxg_ntstatus_to_errno(create->status);
        if (ret != 0) {
            hvdxg.createhwqueue_last_ret = ret;
            goto createhwqueue_done;
        }
        if (actual_len < sizeof(*create) - 1 || create->hwqueue.v == 0) {
            ret = -EIO;
            hvdxg.createhwqueue_last_ret = ret;
            goto createhwqueue_done;
        }
        req.queue.v = create->hwqueue.v;
        req.queue_progress_fence.v = create->hwqueue_progress_fence.v;
        req.queue_progress_fence_gpu_va =
            create->hwqueue_progress_fence_gpuva;
        req.queue_progress_fence_cpu_va =
            create->hwqueue_progress_fence_cpuva != 0 ?
            hvdxg_map_iospace_user(create->hwqueue_progress_fence_cpuva,
                                   PGSIZE, 0) : 0;
        if (create->hwqueue_progress_fence_cpuva != 0)
            fence_kva = hvdxg_map_iospace_kernel(
                create->hwqueue_progress_fence_cpuva, PGSIZE);
        hvdxg.createhwqueue_last_queue = req.queue.v;
        hvdxg.createhwqueue_last_fence = req.queue_progress_fence.v;
        hvdxg.createhwqueue_last_fence_cpu = req.queue_progress_fence_cpu_va;
        hvdxg.createhwqueue_last_fence_gpu = req.queue_progress_fence_gpu_va;
        if (create->hwqueue_progress_fence_cpuva != 0 &&
            req.queue_progress_fence_cpu_va == 0) {
            ret = -ENOMEM;
            hvdxg.createhwqueue_last_ret = ret;
            goto createhwqueue_done;
        }
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1)
            hvdxg_save_priv_head(hvdxg.createhwqueue_last_priv_head,
                                 sizeof(hvdxg.createhwqueue_last_priv_head),
                                 &hvdxg.createhwqueue_last_priv_head_len,
                                 create->priv_drv_data,
                                 req.priv_drv_data_size);
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1 &&
            either_copyout(1, req.priv_drv_data, create->priv_drv_data,
                           req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto createhwqueue_done;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.createhwqueue_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            hvdxg_track_hwqueue(owner, req.queue.v,
                                req.queue_progress_fence.v);
            hvdxg_track_sync(owner, 0, req.queue_progress_fence.v,
                             _D3DDDI_MONITORED_FENCE,
                             0, req.queue_progress_fence.v,
                             req.queue_progress_fence_cpu_va, fence_kva);
        }
createhwqueue_done:
        if (command_buf)
            kvfree(command_buf);
        break;
    }

    case LX_DXDESTROYHWQUEUE: {
        struct d3dkmt_destroyhwqueue req;
        struct hvdxg_command_destroyhwqueue destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.queue.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_hwqueue(owner, req.queue.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYHWQUEUE,
                                        hvdxg.dxg_process);
        destroy.hwqueue.v = req.queue.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.destroyhwqueue_last_len = actual_len;
        hvdxg.destroyhwqueue_last_ret = ret;
        if (ret == 0 && owner != NULL) {
            uint32 sync = hvdxg_untrack_hwqueue(owner, req.queue.v);
            if (sync != 0)
                hvdxg_untrack_sync(owner, sync);
        }
        break;
    }

    case LX_DXDESTROYCONTEXT: {
        struct d3dkmt_destroycontext req;
        struct hvdxg_command_destroycontext destroy;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.context.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_context(owner, req.context.v)) {
            ret = -EPERM;
            break;
        }
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYCONTEXT,
                                        hvdxg.dxg_process);
        destroy.context.v = req.context.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        if (ret == 0 && owner != NULL)
            hvdxg_untrack_u32(owner->contexts, &owner->context_count,
                              req.context.v);
        break;
    }

    case LX_DXESCAPE: {
        struct d3dkmt_escape req;
        uint8 command_buf[sizeof(struct hvdxg_command_escape) +
                          HV_DXG_IOCTL_PRIVATE_MAX - 1];
        uint8 result_buf[HV_DXG_IOCTL_PRIVATE_MAX];
        struct hvdxg_command_escape *escape =
            (struct hvdxg_command_escape *)command_buf;
        uint32 command_len;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.escape_last_len = 0;
        hvdxg.escape_last_ret = 0;
        hvdxg.escape_last_type = (uint32)req.type;
        hvdxg.escape_last_flags = req.flags.value;
        hvdxg.escape_last_size = req.priv_drv_data_size;
        if (req.adapter.v != hvdxg.host_adapter_handle ||
            req.priv_drv_data_size > HV_DXG_IOCTL_PRIVATE_MAX ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            hvdxg.escape_last_ret = ret;
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        memset(result_buf, 0, sizeof(result_buf));
        hvdxg_command_vgpu_init_process(
            &escape->hdr, HV_DXGK_VMBCOMMAND_ESCAPE, hvdxg.dxg_process);
        escape->adapter.v = req.adapter.v;
        escape->device.v = req.device.v;
        escape->type = req.type;
        escape->flags = req.flags;
        escape->priv_drv_data_size = req.priv_drv_data_size;
        escape->context.v = req.context.v;
        if (req.priv_drv_data_size != 0 &&
            either_copyin(escape->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            break;
        }
        command_len = sizeof(*escape) - 1 + req.priv_drv_data_size;
        ret = hvdxg_send_sync_vgpu(escape, command_len, result_buf,
                                   req.priv_drv_data_size, &actual_len);
        if (ret == 0 && req.priv_drv_data_size != 0 && actual_len != 0) {
            uint32 copy_len = actual_len;
            if (copy_len > req.priv_drv_data_size)
                copy_len = req.priv_drv_data_size;
            ret = either_copyout(1, req.priv_drv_data, result_buf,
                                 copy_len) < 0 ? -EFAULT : 0;
        }
        hvdxg.escape_last_len = actual_len;
        hvdxg.escape_last_ret = ret;
        break;
    }

    case LX_DXSHAREOBJECTS: {
        struct d3dkmt_shareobjects req;
        struct hvdxg_d3dkmthandle object;
        struct hvdxg_shared_object *shared = NULL;
        struct hvdxg_tracked_resource *resource = NULL;
        uint32 host_nt_handle = 0;
        uint32 device = 0;
        uint32 kind = 0;
        int fd = -1;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        memset(&req, 0, sizeof(req));
        memset(&object, 0, sizeof(object));
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_device = 0;
        hvdxg.sharedhandle_last_object = 0;
        hvdxg.sharedhandle_last_nt_handle = 0;
        hvdxg.sharedhandle_last_count = req.object_count;
        if (req.object_count != 1 || req.objects == 0 ||
            req.shared_handle == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        if (either_copyin(&object, 1, req.objects, sizeof(object)) < 0) {
            ret = -EFAULT;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        hvdxg.sharedhandle_last_object = object.v;
        if (object.v == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        resource = hvdxg_owner_find_resource(owner, 0, object.v);
        if (resource != NULL) {
            if (!resource->create_shared ||
                !resource->nt_security_sharing) {
                ret = -EINVAL;
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            kind = HV_DXG_SHARED_OBJECT_RESOURCE;
            device = resource->device;
        } else if (hvdxg_owner_has_sync(owner, object.v)) {
            uint32 sync_flags = hvdxg_owner_sync_flags(owner, object.v);

            if ((sync_flags & 0x3U) != 0x3U) {
                ret = -EINVAL;
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            kind = HV_DXG_SHARED_OBJECT_SYNC;
            device = hvdxg_owner_sync_device(owner, object.v);
        } else {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        if (device == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        ret = hvdxg_create_nt_shared_object(object.v, &host_nt_handle);
        if (ret != 0 && kind == HV_DXG_SHARED_OBJECT_RESOURCE) {
            uint64 vail_nt_handle = 0;
            uint32 share_len = 0;
            int32 share_status = 0;

            ret = hvdxg_share_object_with_host(device, object.v, 0,
                                               &vail_nt_handle, &share_len,
                                               &share_status);
            hvdxg.shareobject_last_len = share_len;
            hvdxg.shareobject_last_ret = ret;
            hvdxg.shareobject_last_status = share_status;
            hvdxg.shareobject_last_device = device;
            hvdxg.shareobject_last_object = object.v;
            hvdxg.shareobject_last_nt_handle = vail_nt_handle;
            if (ret == 0 && vail_nt_handle != 0 &&
                vail_nt_handle <= 0xffffffffULL) {
                host_nt_handle = (uint32)vail_nt_handle;
            } else if (ret == 0) {
                ret = -EOVERFLOW;
            }
        }
        if (ret != 0) {
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        shared = kvmalloc(sizeof(*shared));
        if (shared == NULL) {
            (void)hvdxg_destroy_nt_shared_object(host_nt_handle);
            ret = -ENOMEM;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        memset(shared, 0, sizeof(*shared));
        shared->kind = kind;
        shared->device = device;
        shared->object = object.v;
        if (kind == HV_DXG_SHARED_OBJECT_SYNC)
            shared->global_share =
                hvdxg_owner_sync_global_shared(owner, object.v);
        else
            shared->global_share = host_nt_handle;
        shared->host_nt_handle = host_nt_handle;
        shared->nt_handle = host_nt_handle;
        if (kind == HV_DXG_SHARED_OBJECT_SYNC) {
            if (shared->global_share == 0) {
                (void)hvdxg_destroy_nt_shared_object(host_nt_handle);
                kvfree(shared);
                ret = -EINVAL;
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
            shared->sync_type = hvdxg_owner_sync_type(owner, object.v);
        } else {
            ret = hvdxg_clone_resource(&shared->resource, resource);
            if (ret != 0) {
                (void)hvdxg_destroy_nt_shared_object(host_nt_handle);
                kvfree(shared);
                hvdxg.sharedhandle_last_ret = ret;
                break;
            }
        }
        fd = vfs_custom_fd_alloc(&hvdxg_shared_object_file_ops, shared,
                                 O_RDWR);
        if (fd < 0) {
            (void)hvdxg_destroy_nt_shared_object(host_nt_handle);
            if (kind == HV_DXG_SHARED_OBJECT_RESOURCE)
                hvdxg_free_tracked_resource(&shared->resource);
            kvfree(shared);
            ret = fd;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        {
            uint64 fd64 = (uint64)fd;

            ret = either_copyout(1, req.shared_handle, &fd64,
                                 sizeof(fd64)) < 0 ? -EFAULT : 0;
        }
        if (ret != 0) {
            struct vfs_file *f;

            spin_lock(&current->fdtable->lock);
            f = vfs_fdtable_dealloc_fd(current->fdtable, fd);
            spin_unlock(&current->fdtable->lock);
            if (f != NULL)
                vfs_fput(f);
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        hvdxg.sharedhandle_last_device = device;
        hvdxg.sharedhandle_last_nt_handle = host_nt_handle;
        hvdxg.sharedhandle_last_ret = ret;
        break;
    }

    case LX_DXOPENSYNCOBJECTFROMNTHANDLE2: {
        struct d3dkmt_opensyncobjectfromnthandle2 req;
        struct hvdxg_shared_object *shared;
        struct vfs_file *shared_file = NULL;
        struct hvdxg_command_opensyncobject open;
        struct hvdxg_command_opensyncobject_return result;
        uint32 actual_len = 0;
        uint64 fence_kva = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = -ENOTSUP;
        hvdxg.sharedhandle_last_device = req.device.v;
        hvdxg.sharedhandle_last_object = req.sync_object.v;
        hvdxg.sharedhandle_last_nt_handle = req.nt_handle;
        hvdxg.sharedhandle_last_count = 1;
        if (req.device.v == 0 || !hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        shared = hvdxg_shared_object_from_fd((int)req.nt_handle,
                                             HV_DXG_SHARED_OBJECT_SYNC,
                                             &shared_file);
        if (shared == NULL || shared->global_share == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            goto opensync_done;
        }
        memset(&open, 0, sizeof(open));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vm_init(&open.hdr,
                              HV_DXGK_VMBCOMMAND_OPENSYNCOBJECT);
        open.hdr.process = hvdxg.dxg_process;
        open.device.v = req.device.v;
        open.global_sync_object.v = shared->global_share;
        open.flags = req.flags;
        open.flags.shared = 1;
        open.flags.nt_security_sharing = 1;
        if (shared->sync_type == _D3DDDI_MONITORED_FENCE)
            open.engine_affinity = req.monitored_fence.engine_affinity;
        ret = hvdxg_send_sync_global(&open, sizeof(open), &result,
                                     sizeof(result), &actual_len);
        if (ret == 0 && actual_len < sizeof(result))
            ret = -EOVERFLOW;
        if (ret == 0)
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret != 0) {
            hvdxg.sharedhandle_last_ret = ret;
            goto opensync_done;
        }
        req.sync_object.v = result.sync_object.v;
        if (shared->sync_type == _D3DDDI_MONITORED_FENCE) {
            req.monitored_fence.fence_value_cpu_va =
                hvdxg_map_iospace_user(result.guest_cpu_physical_address,
                                       PGSIZE, 0);
            fence_kva =
                hvdxg_map_iospace_kernel(result.guest_cpu_physical_address,
                                         PGSIZE);
            if (req.monitored_fence.fence_value_cpu_va == 0) {
                ret = -ENOMEM;
                hvdxg.sharedhandle_last_ret = ret;
                goto opensync_done;
            }
            req.monitored_fence.fence_value_gpu_va =
                result.gpu_virtual_address;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL)
            hvdxg_track_sync(owner, req.device.v, req.sync_object.v,
                             shared->sync_type, req.flags.value,
                             shared->global_share,
                             req.monitored_fence.fence_value_cpu_va,
                             fence_kva);
        hvdxg.sharedhandle_last_object = req.sync_object.v;
        hvdxg.sharedhandle_last_ret = ret;
opensync_done:
        if (shared_file != NULL)
            vfs_fput(shared_file);
        break;
    }

    case LX_DXQUERYRESOURCEINFOFROMNTHANDLE: {
        struct d3dkmt_queryresourceinfofromnthandle req;
        struct hvdxg_shared_object *shared;
        struct vfs_file *shared_file = NULL;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = -ENOTSUP;
        hvdxg.sharedhandle_last_device = req.device.v;
        hvdxg.sharedhandle_last_object = 0;
        hvdxg.sharedhandle_last_nt_handle = req.nt_handle;
        hvdxg.sharedhandle_last_count = req.allocation_count;
        if (req.device.v == 0 || !hvdxg_owner_has_device(owner, req.device.v)) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        shared = hvdxg_shared_object_from_fd((int)req.nt_handle,
                                             HV_DXG_SHARED_OBJECT_RESOURCE,
                                             &shared_file);
        if (shared == NULL) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        req.private_runtime_data_size =
            shared->resource.private_runtime_data_size;
        req.resource_priv_drv_data_size =
            shared->resource.resource_priv_drv_data_size;
        req.total_priv_drv_data_size =
            shared->resource.total_priv_drv_data_size;
        req.allocation_count = shared->resource.allocation_count;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        hvdxg.sharedhandle_last_count = req.allocation_count;
        hvdxg.sharedhandle_last_object = shared->object;
        hvdxg.sharedhandle_last_ret = ret;
        if (shared_file != NULL)
            vfs_fput(shared_file);
        break;
    }

    case LX_DXOPENRESOURCEFROMNTHANDLE: {
        struct d3dkmt_openresourcefromnthandle req;
        struct hvdxg_shared_object *shared;
        struct hvdxg_command_openresource open;
        struct hvdxg_command_openresource_return *result = NULL;
        struct d3dddi_openallocationinfo2 open_alloc[HV_DXG_ALLOCATION_MAX];
        struct vfs_file *shared_file = NULL;
        uint32 result_len;
        uint32 actual_len = 0;
        struct hvdxg_tracked_resource opened;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.sharedhandle_last_cmd = (uint32)cmd;
        hvdxg.sharedhandle_last_ret = -ENOTSUP;
        hvdxg.sharedhandle_last_device = req.device.v;
        hvdxg.sharedhandle_last_object = req.resource.v;
        hvdxg.sharedhandle_last_nt_handle = req.nt_handle;
        hvdxg.sharedhandle_last_count = req.allocation_count;
        if (req.device.v == 0 || !hvdxg_owner_has_device(owner, req.device.v) ||
            req.allocation_count == 0 ||
            req.allocation_count > HV_DXG_ALLOCATION_MAX ||
            req.open_alloc_info == 0) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        shared = hvdxg_shared_object_from_fd((int)req.nt_handle,
                                             HV_DXG_SHARED_OBJECT_RESOURCE,
                                             &shared_file);
        if (shared == NULL || shared->global_share == 0 ||
            req.allocation_count != shared->resource.allocation_count ||
            req.private_runtime_data_size <
                (int32)shared->resource.private_runtime_data_size ||
            req.resource_priv_drv_data_size <
                shared->resource.resource_priv_drv_data_size ||
            req.total_priv_drv_data_size <
                shared->resource.total_priv_drv_data_size) {
            ret = -EINVAL;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        result_len = sizeof(*result) +
                     (req.allocation_count - 1) *
                         sizeof(result->allocations[0]);
        result = kvmalloc(result_len);
        if (result == NULL) {
            ret = -ENOMEM;
            hvdxg.sharedhandle_last_ret = ret;
            break;
        }
        memset(&open, 0, sizeof(open));
        memset(result, 0, result_len);
        hvdxg_command_vgpu_init_process(&open.hdr,
                                        HV_DXGK_VMBCOMMAND_OPENRESOURCE,
                                        hvdxg.dxg_process);
        open.device.v = req.device.v;
        open.nt_security_sharing = 1;
        open.global_share.v = shared->global_share;
        open.allocation_count = req.allocation_count;
        open.total_priv_drv_data_size =
            shared->resource.total_priv_drv_data_size;
        ret = hvdxg_send_sync_vgpu(&open, sizeof(open), result,
                                   result_len, &actual_len);
        if (ret == 0 && actual_len < result_len)
            ret = -EOVERFLOW;
        if (ret == 0)
            ret = hvdxg_ntstatus_to_errno(result->status);
        if (ret != 0)
            goto openresource_done;
        memset(open_alloc, 0, sizeof(open_alloc));
        for (uint32 i = 0; i < req.allocation_count; i++) {
            open_alloc[i].allocation.v = result->allocations[i].v;
            open_alloc[i].priv_drv_data_size =
                shared->resource.alloc_priv_sizes[i];
        }
        if (shared->resource.private_runtime_data_size != 0 &&
            either_copyout(1, req.private_runtime_data,
                           shared->resource.private_runtime_data,
                           shared->resource.private_runtime_data_size) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        if (shared->resource.resource_priv_drv_data_size != 0 &&
            either_copyout(1, req.resource_priv_drv_data,
                           shared->resource.resource_priv_drv_data,
                           shared->resource.resource_priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        if (shared->resource.total_priv_drv_data_size != 0 &&
            either_copyout(1, req.total_priv_drv_data,
                           shared->resource.total_priv_drv_data,
                           shared->resource.total_priv_drv_data_size) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        if (either_copyout(1, req.open_alloc_info, open_alloc,
                           req.allocation_count * sizeof(open_alloc[0])) < 0) {
            ret = -EFAULT;
            goto openresource_done;
        }
        req.resource.v = result->resource.v;
        req.total_priv_drv_data_size =
            shared->resource.total_priv_drv_data_size;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        if (ret == 0 && owner != NULL) {
            if (hvdxg_clone_resource(&opened, &shared->resource) == 0) {
                struct hvdxg_tracked_resource *slot;

                opened.device = req.device.v;
                opened.resource = req.resource.v;
                slot = hvdxg_owner_find_resource(owner, opened.device,
                                                 opened.resource);
                if (slot == NULL &&
                    hvdxg_grow_table((void **)&owner->resources,
                                     &owner->resource_capacity,
                                     owner->resource_count + 1,
                                     sizeof(owner->resources[0]),
                                     HV_DXG_RESOURCE_TRACKED_MAX) == 0)
                    slot = &owner->resources[owner->resource_count++];
                if (slot != NULL) {
                    hvdxg_free_tracked_resource(slot);
                    *slot = opened;
                } else {
                    hvdxg_free_tracked_resource(&opened);
                }
            }
            for (uint32 i = 0; i < req.allocation_count; i++)
                hvdxg_track_allocation(owner, req.device.v, req.resource.v,
                                       result->allocations[i].v,
                                       shared->resource.allocation_sizes[i],
                                       shared->resource.allocation_flags[i],
                                       0, NULL, 0);
        }
openresource_done:
        if (shared_file != NULL)
            vfs_fput(shared_file);
        if (result != NULL)
            kvfree(result);
        hvdxg.sharedhandle_last_object = req.resource.v;
        hvdxg.sharedhandle_last_ret = ret;
        break;
    }

    case LX_DXSHAREOBJECTWITHHOST: {
        struct d3dkmt_shareobjectwithhost req;
        uint32 actual_len = 0;
        int32 status = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.shareobject_last_len = 0;
        hvdxg.shareobject_last_ret = 0;
        hvdxg.shareobject_last_status = 0;
        hvdxg.shareobject_last_device = req.device_handle.v;
        hvdxg.shareobject_last_object = req.object_handle.v;
        hvdxg.shareobject_last_nt_handle = 0;
        if (req.device_handle.v == 0 || req.object_handle.v == 0) {
            ret = -EINVAL;
            hvdxg.shareobject_last_ret = ret;
            break;
        }
        ret = hvdxg_share_object_with_host(req.device_handle.v,
                                           req.object_handle.v,
                                           req.reserved,
                                           &req.object_vail_nt_handle,
                                           &actual_len, &status);
        hvdxg.shareobject_last_len = actual_len;
        hvdxg.shareobject_last_ret = ret;
        hvdxg.shareobject_last_status = status;
        hvdxg.shareobject_last_nt_handle = req.object_vail_nt_handle;
        if (ret == 0) {
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
                ret = -EFAULT;
        }
        hvdxg.shareobject_last_ret = ret;
        break;
    }

    case LX_DXQUERYVIDEOMEMORYINFO: {
        struct d3dkmt_queryvideomemoryinfo req;
        struct hvdxg_command_queryvideomemoryinfo query;
        struct hvdxg_command_queryvideomemoryinfo_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.process != 0 || req.adapter.v != hvdxg.host_adapter_handle) {
            ret = -EINVAL;
            break;
        }
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&query.hdr,
                                        HV_DXGK_VMBCOMMAND_QUERYVIDEOMEMORYINFO,
                                        hvdxg.dxg_process);
        query.adapter.v = req.adapter.v;
        query.memory_segment_group = (uint32)req.memory_segment_group;
        query.physical_adapter_index = req.physical_adapter_index;
        ret = hvdxg_send_sync_vgpu(&query, sizeof(query), &result,
                                   sizeof(result), &actual_len);
        if (ret != 0)
            break;
        if (actual_len < sizeof(result)) {
            ret = -EOVERFLOW;
            break;
        }
        req.budget = result.budget;
        req.current_usage = result.current_usage;
        req.current_reservation = result.current_reservation;
        req.available_for_reservation = result.available_for_reservation;
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        break;
    }

    case LX_DXQUERYSTATISTICS: {
        struct d3dkmt_querystatistics req;
        struct hvdxg_command_querystatistics query;
        struct hvdxg_command_querystatistics_return result;
        struct hvdxg_winluid luid;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.statistics_last_type = (uint32)req.type;
        hvdxg.statistics_last_len = 0;
        hvdxg.statistics_last_ret = 0;
        hvdxg.statistics_last_status = 0;
        luid.a = req.adapter_luid.a;
        luid.b = req.adapter_luid.b;
        if (req.process != 0 || !hvdxg_luid_equal(luid, hvdxg.adapter_luid)) {
            ret = -EINVAL;
            hvdxg.statistics_last_ret = ret;
            break;
        }
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(&query.hdr,
                                        HV_DXGK_VMBCOMMAND_QUERYSTATISTICS,
                                        hvdxg.dxg_process);
        query.args = req;
        query.args.adapter_luid.a = hvdxg.host_adapter_luid.a;
        query.args.adapter_luid.b = hvdxg.host_adapter_luid.b;
        ret = hvdxg_send_sync_vgpu(&query, sizeof(query), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.statistics_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.result = result.result;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        hvdxg.statistics_last_len = actual_len;
        hvdxg.statistics_last_ret = ret;
        break;
    }

    case LX_DXCREATESYNCFILE: {
        struct d3dkmt_createsyncfile req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.syncfile_last_cmd = (uint32)cmd;
        hvdxg.syncfile_last_device = req.device.v;
        hvdxg.syncfile_last_object = req.monitored_fence.v;
        hvdxg.syncfile_last_context = 0;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_fence = req.fence_value;
        ret = -ENOTSUP;
        hvdxg.syncfile_last_ret = ret;
        break;
    }

    case LX_DXWAITSYNCFILE: {
        struct d3dkmt_waitsyncfile req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.syncfile_last_cmd = (uint32)cmd;
        hvdxg.syncfile_last_device = 0;
        hvdxg.syncfile_last_object = 0;
        hvdxg.syncfile_last_context = req.context.v;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_fence = 0;
        ret = -ENOTSUP;
        hvdxg.syncfile_last_ret = ret;
        break;
    }

    case LX_DXOPENSYNCOBJECTFROMSYNCFILE: {
        struct d3dkmt_opensyncobjectfromsyncfile req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.syncfile_last_cmd = (uint32)cmd;
        hvdxg.syncfile_last_device = req.device.v;
        hvdxg.syncfile_last_object = req.syncobj.v;
        hvdxg.syncfile_last_context = 0;
        hvdxg.syncfile_last_handle = req.sync_file_handle;
        hvdxg.syncfile_last_fence = req.fence_value;
        ret = -ENOTSUP;
        hvdxg.syncfile_last_ret = ret;
        break;
    }

    case LX_DXCHANGEVIDEOMEMORYRESERVATION: {
        struct d3dkmt_changevideomemoryreservation req;
        struct d3dkmt_changevideomemoryreservation wire_req;
        struct hvdxg_command_changevideomemoryreservation change;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.process != 0 || req.adapter.v != hvdxg.host_adapter_handle) {
            ret = -EINVAL;
            break;
        }
        wire_req = req;
        wire_req.adapter.v = 0;
        wire_req.process = 0;
        memset(&change, 0, sizeof(change));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &change.hdr, HV_DXGK_VMBCOMMAND_CHANGEVIDEOMEMORYRESERVATION,
            hvdxg.dxg_process);
        change.args = wire_req;
        ret = hvdxg_send_sync_vgpu(&change, sizeof(change), &status,
                                   sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.vidmem_reservation_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.vidmem_reservation_last_len = actual_len;
        hvdxg.vidmem_reservation_last_ret = ret;
        hvdxg.vidmem_reservation_last_group = req.memory_segment_group;
        hvdxg.vidmem_reservation_last_value = req.reservation;
        break;
    }

    case LX_DXMARKDEVICEASERROR: {
        struct d3dkmt_markdeviceaserror req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg_note_unsupported_ioctl((uint32)cmd, req.device.v, 0,
                                     (uint32)req.reason);
        ret = -ENOTSUP;
        break;
    }

    case LX_DXSUBMITSIGNALSYNCOBJECTSTOHWQUEUE: {
        struct d3dkmt_submitsignalsyncobjectstohwqueue req;
        uint8 command_buf[sizeof(struct hvdxg_command_signalsyncobject) +
                          D3DDDI_MAX_OBJECT_SIGNALED *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64)) +
                          D3DDDI_MAX_BROADCAST_CONTEXT *
                              sizeof(struct hvdxg_d3dkmthandle)];
        struct hvdxg_command_signalsyncobject *signal =
            (struct hvdxg_command_signalsyncobject *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle hwqueues[D3DDDI_MAX_BROADCAST_CONTEXT];
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_SIGNALED];
        uint64 fences[D3DDDI_MAX_OBJECT_SIGNALED];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 hwqueue_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.hwqueue_count == 0 ||
            req.hwqueue_count > D3DDDI_MAX_BROADCAST_CONTEXT ||
            req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_SIGNALED ||
            req.hwqueues == 0 || req.objects == 0 ||
            req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        hwqueue_size = req.hwqueue_count *
                       sizeof(struct hvdxg_d3dkmthandle);
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        if (either_copyin(hwqueues, 1, req.hwqueues, hwqueue_size) < 0 ||
            either_copyin(objects, 1, req.objects, object_size) < 0 ||
            either_copyin(fences, 1, req.fence_values, fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        for (uint32 i = 0; i < req.hwqueue_count; i++) {
            if (!hvdxg_owner_has_hwqueue(owner, hwqueues[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        for (uint32 i = 0; ret == 0 && i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_signal_last_status = 0;
        hvdxg_command_vgpu_init_process(&signal->hdr,
                                        HV_DXGK_VMBCOMMAND_SIGNALSYNCOBJECT,
                                        hvdxg.dxg_process);
        signal->object_count = req.object_count;
        signal->flags = req.flags;
        signal->context_count = req.hwqueue_count;
        signal->fence_value = 0;
        signal->u.device.v = 0;
        pos = (uint8 *)&signal[1];
        memcpy(pos, objects, object_size);
        pos += object_size;
        memcpy(pos, hwqueues, hwqueue_size);
        pos += hwqueue_size;
        memcpy(pos, fences, fence_size);
        ret = hvdxg_send_sync_vgpu(signal,
                                   sizeof(*signal) + object_size +
                                       hwqueue_size + fence_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_signal_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_signal_last_len = actual_len;
        hvdxg.syncgpu_signal_last_ret = ret;
        break;
    }

    case LX_DXSUBMITWAITFORSYNCOBJECTSTOHWQUEUE: {
        struct d3dkmt_submitwaitforsyncobjectstohwqueue req;
        uint8 command_buf[sizeof(struct hvdxg_command_waitsyncobjectfromgpu) +
                          D3DDDI_MAX_OBJECT_WAITED_ON *
                              (sizeof(struct hvdxg_d3dkmthandle) +
                               sizeof(uint64))];
        struct hvdxg_command_waitsyncobjectfromgpu *wait =
            (struct hvdxg_command_waitsyncobjectfromgpu *)command_buf;
        struct hvdxg_ntstatus status;
        struct hvdxg_d3dkmthandle objects[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint64 fences[D3DDDI_MAX_OBJECT_WAITED_ON];
        uint32 actual_len = 0;
        uint32 object_size;
        uint32 fence_size;
        uint8 *pos;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.hwqueue.v == 0 || req.object_count == 0 ||
            req.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
            req.objects == 0 || req.fence_values == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_hwqueue(owner, req.hwqueue.v)) {
            ret = -EPERM;
            break;
        }
        object_size = req.object_count * sizeof(struct hvdxg_d3dkmthandle);
        fence_size = req.object_count * sizeof(uint64);
        if (either_copyin(objects, 1, req.objects, object_size) < 0 ||
            either_copyin(fences, 1, req.fence_values, fence_size) < 0) {
            ret = -EFAULT;
            break;
        }
        for (uint32 i = 0; i < req.object_count; i++) {
            if (!hvdxg_owner_has_sync(owner, objects[i].v)) {
                ret = -EPERM;
                break;
            }
        }
        if (ret != 0)
            break;
        memset(command_buf, 0, sizeof(command_buf));
        memset(&status, 0, sizeof(status));
        hvdxg.syncgpu_wait_last_status = 0;
        hvdxg_command_vgpu_init_process(
            &wait->hdr, HV_DXGK_VMBCOMMAND_WAITFORSYNCOBJECTFROMGPU,
            hvdxg.dxg_process);
        wait->context.v = req.hwqueue.v;
        wait->object_count = req.object_count;
        wait->legacy_fence_object = 0;
        pos = (uint8 *)wait->fence_values;
        memcpy(pos, fences, fence_size);
        pos += fence_size;
        memcpy(pos, objects, object_size);
        ret = hvdxg_send_sync_vgpu(wait,
                                   sizeof(*wait) - sizeof(uint64) +
                                       fence_size + object_size,
                                   &status, sizeof(status), &actual_len);
        if (actual_len >= sizeof(status))
            hvdxg.syncgpu_wait_last_status = status.v;
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.syncgpu_wait_last_len = actual_len;
        hvdxg.syncgpu_wait_last_ret = ret;
        break;
    }

    case LX_DXUPDATEALLOCPROPERTY: {
        struct d3dddi_updateallocproperty req;
        struct hvdxg_command_updateallocationproperty update;
        struct hvdxg_command_updateallocationproperty_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.paging_queue.v == 0 || req.allocation.v == 0) {
            ret = -EINVAL;
            break;
        }
        if (!hvdxg_owner_has_pagingqueue(owner, req.paging_queue.v) ||
            !hvdxg_owner_has_allocation(owner, 0, 0, req.allocation.v)) {
            ret = -EPERM;
            break;
        }
        memset(&update, 0, sizeof(update));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &update.hdr, HV_DXGK_VMBCOMMAND_UPDATEALLOCATIONPROPERTY,
            hvdxg.dxg_process);
        update.args = req;
        ret = hvdxg_send_sync_vgpu(&update, sizeof(update), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result))
            hvdxg.updateallocproperty_last_fence =
                result.paging_fence_value;
        if (actual_len >= sizeof(result.paging_fence_value) +
                          sizeof(result.status))
            hvdxg.updateallocproperty_last_status = result.status.v;
        if (ret == 0 &&
            actual_len >= sizeof(result.paging_fence_value) +
                          sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        hvdxg.updateallocproperty_last_len = actual_len;
        hvdxg.updateallocproperty_last_ret = ret;
        hvdxg.updateallocproperty_last_allocation = req.allocation.v;
        if ((ret == 0 || ret == 259) && actual_len >= sizeof(result)) {
            req.paging_fence_value = result.paging_fence_value;
            if (either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0)
                ret = -EFAULT;
        }
        break;
    }

    case LX_DXQUERYCLOCKCALIBRATION: {
        struct d3dkmt_queryclockcalibration req;
        struct d3dkmt_queryclockcalibration wire_req;
        struct hvdxg_command_queryclockcalibration query;
        struct hvdxg_command_queryclockcalibration_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapter.v != hvdxg.host_adapter_handle) {
            ret = -EINVAL;
            break;
        }
        wire_req = req;
        wire_req.adapter.v = hvdxg.host_adapter_handle;
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        hvdxg_command_vgpu_init_process(
            &query.hdr, HV_DXGK_VMBCOMMAND_QUERYCLOCKCALIBRATION,
            hvdxg.dxg_process);
        query.args = wire_req;
        ret = hvdxg_send_sync_vgpu(&query, sizeof(query), &result,
                                   sizeof(result), &actual_len);
        if (actual_len >= sizeof(result.status))
            hvdxg.clockcalibration_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result))
            ret = hvdxg_ntstatus_to_errno(result.status);
        hvdxg.clockcalibration_last_len = actual_len;
        hvdxg.clockcalibration_last_ret = ret;
        hvdxg.clockcalibration_last_node = req.node_ordinal;
        hvdxg.clockcalibration_last_gpu_frequency =
            result.clock_data.gpu_frequency;
        hvdxg.clockcalibration_last_gpu_counter =
            result.clock_data.gpu_clock_counter;
        hvdxg.clockcalibration_last_cpu_counter =
            result.clock_data.cpu_clock_counter;
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.clock_data = result.clock_data;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        break;
    }

    case LX_DXENUMPROCESSES: {
        struct d3dkmt_enumprocesses req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg_note_unsupported_ioctl((uint32)cmd, 0, 0,
                                     (uint32)req.buffer_count);
        ret = -ENOTSUP;
        break;
    }

    case LX_DXCLOSEADAPTER: {
        struct d3dkmt_closeadapter req;

        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = (req.adapter_handle.v == hvdxg.host_adapter_handle) ? 0 :
              -EINVAL;
        break;
    }

    case LX_DXQUERYADAPTERINFO: {
        ret = hvdxg_ioctl_queryadapterinfo((uint64)arg);
        break;
    }

    case LX_ISFEATUREENABLED: {
        struct d3dkmt_isfeatureenabled req;
        struct hvdxg_command_isfeatureenabled feature;
        struct hvdxg_command_isfeatureenabled_global global_feature;
        struct hvdxg_command_isfeatureenabled_return result;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        hvdxg.feature_last_id = (uint32)req.feature_id;
        hvdxg.feature_last_result = 0;
        hvdxg.feature_last_status = 0;
        memset(&result, 0, sizeof(result));
        if (req.adapter.v == hvdxg.host_adapter_handle) {
            memset(&feature, 0, sizeof(feature));
            hvdxg_command_vgpu_init_process(
                &feature.hdr, HV_DXGK_VMBCOMMAND_ISFEATUREENABLED,
                hvdxg.dxg_process);
            feature.feature_id = req.feature_id;
            ret = hvdxg_send_sync_vgpu(&feature, sizeof(feature), &result,
                                       sizeof(result), &actual_len);
        } else if (req.adapter.v == 0) {
            memset(&global_feature, 0, sizeof(global_feature));
            hvdxg_command_vm_init(
                &global_feature.hdr,
                HV_DXGK_VMBCOMMAND_ISFEATUREENABLED_GLOBAL);
            global_feature.hdr.process = hvdxg.dxg_process;
            global_feature.feature_id = req.feature_id;
            ret = hvdxg_send_sync_global(&global_feature,
                                         sizeof(global_feature), &result,
                                         sizeof(result), &actual_len);
        } else {
            ret = -EINVAL;
            hvdxg.feature_last_ret = ret;
            break;
        }
        if (actual_len >= sizeof(result.status))
            hvdxg.feature_last_status = result.status.v;
        if (ret == 0 && actual_len >= sizeof(result.status))
            ret = hvdxg_ntstatus_to_errno(result.status);
        if (ret == 0 && actual_len >= sizeof(result)) {
            req.result = result.result;
            hvdxg.feature_last_result = req.result.value;
            ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
                  -EFAULT : 0;
        }
        hvdxg.feature_last_len = actual_len;
        hvdxg.feature_last_ret = ret;
        break;
    }

    default:
        ret = -ENOTSUP;
        break;
    }

    hvdxg.ioctl_last_ret = ret;
    hvdxg_note_ioctl_history(cmd, ret);
    hvdxg_note_ioctl_timing(cmd, r_time() - start_ticks);
    if (ret >= 0)
        hvdxg.ioctl_successes++;
    return ret;
}

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
    int ret = hvdxg_open(cdev);

    if (ret != 0)
        return ret;
    owner = kvmalloc(sizeof(*owner));
    if (owner == NULL) {
        (void)hvdxg_release(cdev);
        return -ENOMEM;
    }
    memset(owner, 0, sizeof(*owner));
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
    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
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
    return (hvdxg.global_present || hvdxg.vgpu_present) ? 0 : -ENODEV;
}

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
    uint64 status = 0;

    if (monitor_allocated) {
        hv_clear_channel_signal(child_relid, monitor_allocated, monitorid);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        hv_send_interrupt(child_relid);
        hv_set_monitor_event(monitor_allocated, monitorid);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32 event_conn = signal_conn_id;
        if (event_conn != 0)
            status = hv_do_fast_hypercall8(HVCALL_SIGNAL_EVENT,
                                           event_conn);
        if (hv_debug_enabled() && debug_signal_count < 8) {
            volatile uint64 *bits = (volatile uint64 *)hv.send_int_page;
            struct hv_monitor_page *page =
                (struct hv_monitor_page *)hv.monitor2;
            uint32 group = monitorid / 32;
            uint32 pending = (page && group < 4) ?
                page->trigger_group[group].pending : 0;
            printf("hyperv-vmbus: signal relid=%u conn=%u monitor=%d monid=%u status=0x%lx intword=0x%lx pending=0x%x\n",
                   child_relid, event_conn, monitor_allocated,
                   monitorid, status & 0xffff,
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
                hvdxg.vgpu_instance = offer->offer.if_instance;
            }
            hv_log_offer("gpu-pv-dxg-vgpu", offer);
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

static void hvdxg_capture_completion(const uint8 *payload, uint32 payload_len,
                                     uint16 type, uint64 trans_id)
{
    uint64 waiting = __atomic_load_n(&hvdxg.waiting_trans_id,
                                     __ATOMIC_ACQUIRE);
    uint32 copy_len = payload_len > HV_DXG_RESULT_BYTES ?
                      HV_DXG_RESULT_BYTES : payload_len;
    uint32 prefix_len = payload_len > HV_DXG_PREFIX_BYTES ?
                        HV_DXG_PREFIX_BYTES : payload_len;

    hvdxg.probe_last_type = type;
    hvdxg.probe_last_len = payload_len;
    memset(hvdxg.probe_last_prefix, 0, sizeof(hvdxg.probe_last_prefix));
    if (prefix_len != 0)
        memcpy(hvdxg.probe_last_prefix, payload, prefix_len);

    if (waiting == 0 || trans_id != waiting)
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
        if (type == VM_PKT_COMP || type == VM_PKT_DATA_INBAND)
            hvdxg_capture_completion(payload, payload_len, type,
                                     desc->trans_id);
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
                                      &hvdxg.global_rx_packets);
    if (hvdxg.vgpu_open_ok)
        hvdxg_process_channel_packets(hvdxg.vgpu_in_ring,
                                      &hvdxg.vgpu_rx_packets);
    __atomic_store_n(&hvdxg.pump_active, 0, __ATOMIC_RELEASE);
}

static int hvdxg_wait_host_event(uint64 event_id, uint64 timeout_ms)
{
    for (uint64 i = 0; i < timeout_ms; i++) {
        hvdxg_pump_events_ms(1);
        if (hvdxg_host_event_is_signaled(event_id)) {
            hvdxg.host_event_wait_successes++;
            return 0;
        }
        sleep_ms(1);
    }
    hvdxg.host_event_wait_timeouts++;
    return -ETIMEDOUT;
}

static int hvdxg_send_waitsyncobjectfromcpu(
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
        hvdxg.dxg_process);
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
            __atomic_store_n(&hvdxg.waiting_trans_id, 0,
                             __ATOMIC_RELEASE);
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
            __atomic_store_n(&hvdxg.waiting_trans_id, 0,
                             __ATOMIC_RELEASE);
            return 0;
        }
        sleep_ms(1);
    }
    __atomic_store_n(&hvdxg.waiting_trans_id, 0, __ATOMIC_RELEASE);
    if (actual_len != NULL)
        *actual_len = 0;
    return -ETIMEDOUT;
}

static int hvdxg_send_sync_vgpu(const void *cmd, uint32 cmd_len,
                                void *result, uint32 result_len,
                                uint32 *actual_len)
{
    struct hvdxg_ext_header *ext = NULL;
    uint32 send_len = cmd_len;
    const void *send_cmd = cmd;
    uint64 trans_id;
    int ret;

    if (!hvdxg.vgpu_open_ok || hvdxg.vgpu_out_ring == NULL)
        return -ENODEV;

    if (hvdxg.use_ext_header) {
        send_len = cmd_len + sizeof(*ext);
        ext = kvmalloc(send_len);
        if (ext == NULL)
            return -ENOMEM;
        memset(ext, 0, sizeof(*ext));
        ext->command_offset = sizeof(*ext);
        ext->vgpu_luid = hvdxg.host_vgpu_luid;
        memcpy((uint8 *)ext + sizeof(*ext), cmd, cmd_len);
        send_cmd = ext;
    }

    ret = hvdxg_sync_acquire();
    if (ret != 0) {
        if (ext != NULL)
            kvfree(ext);
        return ret;
    }
    trans_id = hvdxg_next_trans_id();
    __atomic_store_n(&hvdxg.completion_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_trans_id, trans_id, __ATOMIC_RELEASE);
    ret = hv_send_packet_on(hvdxg.vgpu_out_ring, hvdxg.vgpu_relid,
                            hvdxg.vgpu_conn_id,
                            hvdxg.vgpu_monitor_allocated,
                            hvdxg.vgpu_monitorid, hvdxg.vgpu_dedicated,
                            send_cmd, send_len, trans_id,
                            VM_PKT_COMPLETION_REQUESTED);
    if (ret != 0) {
        __atomic_store_n(&hvdxg.waiting_trans_id, 0, __ATOMIC_RELEASE);
        hvdxg_sync_release();
        if (ext != NULL)
            kvfree(ext);
        return ret;
    }
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
    uint64 trans_id;
    int ret;

    if (!hvdxg.global_open_ok || hvdxg.global_out_ring == NULL)
        return -ENODEV;

    ret = hvdxg_sync_acquire();
    if (ret != 0)
        return ret;
    trans_id = hvdxg_next_trans_id();
    __atomic_store_n(&hvdxg.completion_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_trans_id, trans_id, __ATOMIC_RELEASE);
    ret = hv_send_packet_on(hvdxg.global_out_ring, hvdxg.global_relid,
                            hvdxg.global_conn_id,
                            hvdxg.global_monitor_allocated,
                            hvdxg.global_monitorid, hvdxg.global_dedicated,
                            cmd, cmd_len, trans_id,
                            VM_PKT_COMPLETION_REQUESTED);
    if (ret != 0) {
        __atomic_store_n(&hvdxg.waiting_trans_id, 0, __ATOMIC_RELEASE);
        hvdxg_sync_release();
        return ret;
    }
    ret = hvdxg_wait_completion(trans_id, result, result_len, actual_len,
                                HV_DXG_WAIT_MS);
    hvdxg_sync_release();
    return ret;
}

static int hvdxg_create_process(void)
{
    struct hvdxg_command_createprocess cmd;
    struct hvdxg_command_createprocess_return result;
    uint32 actual_len = 0;
    const char *name = current ? current->name : "xv6-dxg";
    int ret;

    if (hvdxg.dxg_process_created && hvdxg.dxg_process.v != 0)
        return 0;

    memset(&cmd, 0, sizeof(cmd));
    hvdxg_command_vm_init(&cmd.hdr, HV_DXGK_VMBCOMMAND_CREATEPROCESS);
    cmd.process = current ? (uint64)current : 1;
    cmd.process_id = current ? (uint64)current->tgid : 1;
    for (uint32 i = 0; i < HV_DXG_PROCESS_NAME_LENGTH && name[i] != 0; i++)
        cmd.process_name[i] = (uint16)name[i];
    cmd.flags = 0x8;

    memset(&result, 0, sizeof(result));
    ret = hvdxg_send_sync_global(&cmd, sizeof(cmd), &result,
                                 sizeof(result), &actual_len);
    if (ret != 0)
        return ret;
    if (actual_len < sizeof(result) || result.hprocess.v == 0)
        return -EIO;
    hvdxg.dxg_process = result.hprocess;
    hvdxg.dxg_process_created = 1;
    return 0;
}

static int hvdxg_d3dkmt_ensure(void)
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
    uint32 versions[2] = {
        HV_DXG_VMBUS_INTERFACE_VERSION_OLD,
        HV_DXG_VMBUS_INTERFACE_VERSION,
    };
    int ret;

    if (!hvdxg.vgpu_open_ok)
        return -ENODEV;

    hvdxg.probe_attempts++;
    hvdxg.probe_last_ret = -EIO;
    hvdxg.probe_open_status = 0;
    hvdxg.probe_open_handle = 0;
    hvdxg.probe_open_host_version = 0;
    hvdxg.probe_open_host_compat = 0;
    hvdxg.probe_info_len = 0;
    hvdxg.probe_info_flags = 0;
    hvdxg.probe_async_msg_enabled = 0;
    hvdxg.use_ext_header = 0;
    hvdxg_apply_cmdline_host_luid();

    for (uint32 i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        memset(&open, 0, sizeof(open));
        hvdxg_command_vgpu_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENADAPTER);
        open.vmbus_interface_version = versions[i];
        open.vmbus_last_compatible_interface_version =
            HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;
        open.guest_adapter_luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);

        memset(&open_ret, 0, sizeof(open_ret));
        actual_len = 0;
        ret = hvdxg_send_sync_vgpu(&open, sizeof(open), &open_ret,
                                   sizeof(open_ret), &actual_len);
        hvdxg.probe_last_ret = ret;
        if (ret != 0)
            continue;
        if (actual_len < sizeof(open_ret)) {
            hvdxg.probe_last_ret = -EOVERFLOW;
            ret = -EOVERFLOW;
            continue;
        }

        hvdxg.probe_open_status = open_ret.status.v;
        hvdxg.probe_open_handle = open_ret.host_adapter_handle.v;
        hvdxg.probe_open_host_version = open_ret.vmbus_interface_version;
        hvdxg.probe_open_host_compat =
            open_ret.vmbus_last_compatible_interface_version;
        hvdxg.adapter_luid = open.guest_adapter_luid;
        hvdxg.host_adapter_handle = open_ret.host_adapter_handle.v;
        if (open_ret.status.v < 0) {
            hvdxg.probe_last_ret = open_ret.status.v;
            ret = -EIO;
            continue;
        }
        if (open_ret.host_adapter_handle.v == 0) {
            hvdxg.probe_last_ret = -EIO;
            ret = -EIO;
            continue;
        }
        ret = 0;
        break;
    }
    if (ret != 0)
        return ret;

    hvdxg.probe_successes++;

    memset(&info, 0, sizeof(info));
    hvdxg_command_vgpu_init(&info.hdr,
                            HV_DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO);
    memset(&info_ret, 0, sizeof(info_ret));
    actual_len = 0;
    info_result_len = sizeof(info_ret);
    if (hvdxg.probe_open_host_version < HV_DXG_VMBUS_INTERFACE_VERSION)
        info_result_len -= sizeof(struct hvdxg_winluid);
    ret = hvdxg_send_sync_vgpu(&info, sizeof(info), &info_ret,
                               info_result_len, &actual_len);
    hvdxg.probe_last_ret = ret;
    if (ret != 0)
        return 0;
    hvdxg.probe_info_len = actual_len;
    if (actual_len >= sizeof(uint32) * 4) {
        hvdxg.probe_info_flags = info_ret.flags;
        hvdxg.probe_async_msg_enabled = (info_ret.flags >> 6) & 1U;
        hvdxg.host_adapter_luid = info_ret.host_adapter_luid;
        if (actual_len >= sizeof(info_ret) &&
            (info_ret.host_vgpu_luid.a != 0 ||
             info_ret.host_vgpu_luid.b != 0))
            hvdxg.host_vgpu_luid = info_ret.host_vgpu_luid;
        else if (hvdxg.pci_host_vgpu_luid.a != 0 ||
                 hvdxg.pci_host_vgpu_luid.b != 0)
            hvdxg.host_vgpu_luid = hvdxg.pci_host_vgpu_luid;
        if (hvdxg.probe_open_host_version >=
                HV_DXG_VMBUS_INTERFACE_VERSION &&
            (hvdxg.host_vgpu_luid.a != 0 || hvdxg.host_vgpu_luid.b != 0))
            hvdxg.use_ext_header = 1;
    }
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
    if (hvdxg.global_open_ok && hvdxg.global_relid != 0 &&
        hv_test_and_clear_event(hvdxg.global_relid))
        hvdxg_pump_channels();
    if (hvdxg.vgpu_open_ok && hvdxg.vgpu_relid != 0 &&
        hv_test_and_clear_event(hvdxg.vgpu_relid))
        hvdxg_pump_channels();
}

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

    if (monitor_allocated || old_write == read)
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

    if (monitor_allocated || old_write == read)
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

#else

void hyperv_input_init(void) {}
void hyperv_input_intr(void) {}
void hyperv_storvsc_init(void) {}
void hyperv_netvsc_init(void) {}
void hyperv_video_dirty(uint32 x, uint32 y, uint32 w, uint32 h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

#endif
