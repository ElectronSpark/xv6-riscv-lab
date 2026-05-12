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
#include <vfs/stat.h>
#include <lock/spinlock.h>
#include <lock/mutex_types.h>
#include <lock/mutex.h>
#include <mm/page.h>
#include <mm/pgtable.h>
#include <mm/vm.h>
#include <proc/thread.h>
#include <proc/sched.h>
#include <cmdline.h>

#if defined(__x86_64__) || defined(__i386__)

#include "seg.h"
#include "x86.h"

extern void sleep_ms(uint64 ms);
int snprintf(char *buf, size_t size, const char *fmt, ...);

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

#define HV_DXG_WAIT_MS 2000
#define HV_DXG_RESULT_BYTES 1024
#define HV_DXG_PREFIX_BYTES 64
#define HV_DXG_VMBUS_INTERFACE_VERSION_OLD 27U
#define HV_DXG_VMBUS_INTERFACE_VERSION 40U
#define HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION 16U
#define HV_DXGK_VMBCOMMAND_CREATEDEVICE 0U
#define HV_DXGK_VMBCOMMAND_DESTROYDEVICE 1U
#define HV_DXGK_VMBCOMMAND_CREATEPROCESS 1000U
#define HV_DXGK_VMBCOMMAND_DESTROYPROCESS 1001U
#define HV_DXGK_VMBCOMMAND_DESTROYSYNCOBJECT 1003U
#define HV_DXGK_VMBCOMMAND_SETIOSPACEREGION 1010U
#define HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO 2U
#define HV_DXGK_VMBCOMMAND_CREATESYNCOBJECT 8U
#define HV_DXGK_VMBCOMMAND_CREATEPAGINGQUEUE 9U
#define HV_DXGK_VMBCOMMAND_DESTROYPAGINGQUEUE 10U
#define HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS 16U
#define HV_DXGK_VMBCOMMAND_RESERVEGPUVIRTUALADDRESS 18U
#define HV_DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL 6U
#define HV_DXGK_VMBCOMMAND_DESTROYCONTEXT 7U
#define HV_DXGK_VMBCOMMAND_GETDEVICESTATE 28U
#define HV_DXGK_VMBCOMMAND_OPENADAPTER 14U
#define HV_DXGK_VMBCOMMAND_CLOSEADAPTER 15U
#define HV_DXGK_VMBCOMMAND_QUERYVIDEOMEMORYINFO 21U
#define HV_DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO 36U
#define HV_DXGKVMB_VM_TO_HOST 1U
#define HV_DXGKVMB_VGPU_TO_HOST 0U
#define HV_DXG_PROCESS_NAME_LENGTH 260U
#define HV_DXG_IOCTL_PRIVATE_MAX 1024U
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

struct hvdxg_command_getdevicestate {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_getdevicestate args;
} __PACKED;

struct hvdxg_command_getdevicestate_return {
    struct d3dkmt_getdevicestate args;
    struct hvdxg_ntstatus status;
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
} __PACKED;

struct hvdxg_command_destroycontext {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle context;
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

struct hvdxg_command_destroysyncobject {
    struct hvdxg_command_vm_to_host hdr;
    struct hvdxg_d3dkmthandle sync_object;
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
    struct hvdxg_d3dkmthandle dxg_process;
    uint32 dxg_process_created;
    uint32 host_adapter_handle;
    struct hvdxg_winluid adapter_luid;
    struct hvdxg_winluid host_adapter_luid;
    struct hvdxg_winluid host_vgpu_luid;
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

static void hvdxg_process_channel_packets(struct hv_ring_buffer *in_ring,
                                          uint32 *counter);
static void hvdxg_command_vgpu_init_process(
    struct hvdxg_command_vgpu_to_host *hdr, uint32 command_type,
    struct hvdxg_d3dkmthandle process);
static int hvdxg_d3dkmt_ensure(void);
static int hvdxg_luid_equal(struct hvdxg_winluid a, struct hvdxg_winluid b);
static int hvdxg_ntstatus_to_errno(struct hvdxg_ntstatus status);
static int hvdxg_send_sync_vgpu(const void *cmd, uint32 cmd_len,
                                void *result, uint32 result_len,
                                uint32 *actual_len);
static int hvdxg_probe_transport(void);

static int hvdxg_open(cdev_t *cdev)
{
    (void)cdev;
    hvdxg.read_emitted = 0;
    return 0;
}

static int hvdxg_release(cdev_t *cdev)
{
    (void)cdev;
    return 0;
}

static int hvdxg_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;
    char status[2048];

    if (hvdxg.read_emitted)
        return 0;
    if (hvdxg.vgpu_open_ok && hvdxg.probe_attempts == 0)
        (void)hvdxg_probe_transport();

    int len = snprintf(status, sizeof(status),
        "hyperv_dxg_global=%d relid=%u conn=%u monitor=%u\n"
        "hyperv_dxg_global_transport=gpadl:%d status:%u open:%d status:%u rx:%u\n"
        "dxg_iospace=offer_mb:%u set:%d ret:%d len:%u base:0x%lx size:0x%lx\n"
        "hyperv_dxg_vgpu=%d count=%d relid=%u conn=%u monitor=%u\n"
        "hyperv_dxg_vgpu_transport=gpadl:%d status:%u open:%d status:%u rx:%u\n"
        "dxg_pagingqueue_last=len:%u ret:%d queue:0x%x sync:0x%x fence_pa:0x%lx fence_off:0x%lx\n"
        "dxg_gpuva_last=reserve_len:%u reserve_ret:%d va:0x%lx fence:%lu free_len:%u free_ret:%d\n"
        "dxg_syncobject_last=len:%u ret:%d handle:0x%x\n"
        "dxg_probe_attempts=%u successes=%u last_ret=%d\n"
        "dxg_probe_open_status=%d handle=0x%x host_version=%u host_compat=%u\n"
        "dxg_probe_info_len=%u flags=0x%x async_msg=%u\n"
        "dxg_probe_last_packet=type:%u len:%u prefix:%02x%02x%02x%02x%02x%02x%02x%02x\n"
        "d3dkmt_ioctls=%u successes=%u ready=%d last_ret=%d process=0x%x\n"
        "note=Hyper-V GPU-PV D3DKMT adapter ioctls are available; "
        "device/context and sync/paging ioctls are wired; allocation/OpenGL submission is still pending.\n",
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
        hvdxg.pagingqueue_last_len, hvdxg.pagingqueue_last_ret,
        hvdxg.pagingqueue_last_queue, hvdxg.pagingqueue_last_sync,
        hvdxg.pagingqueue_last_fence_pa, hvdxg.pagingqueue_last_fence_off,
        hvdxg.gpuva_reserve_last_len, hvdxg.gpuva_reserve_last_ret,
        hvdxg.gpuva_reserve_last_va, hvdxg.gpuva_reserve_last_fence,
        hvdxg.gpuva_free_last_len, hvdxg.gpuva_free_last_ret,
        hvdxg.syncobject_last_len, hvdxg.syncobject_last_ret,
        hvdxg.syncobject_last_handle,
        hvdxg.probe_attempts, hvdxg.probe_successes,
        hvdxg.probe_last_ret, hvdxg.probe_open_status,
        hvdxg.probe_open_handle, hvdxg.probe_open_host_version,
        hvdxg.probe_open_host_compat, hvdxg.probe_info_len,
        hvdxg.probe_info_flags, hvdxg.probe_async_msg_enabled,
        hvdxg.probe_last_type, hvdxg.probe_last_len,
        hvdxg.probe_last_prefix[0], hvdxg.probe_last_prefix[1],
        hvdxg.probe_last_prefix[2], hvdxg.probe_last_prefix[3],
        hvdxg.probe_last_prefix[4], hvdxg.probe_last_prefix[5],
        hvdxg.probe_last_prefix[6], hvdxg.probe_last_prefix[7],
        hvdxg.ioctl_count, hvdxg.ioctl_successes, hvdxg.d3dkmt_ready,
        hvdxg.ioctl_last_ret, hvdxg.dxg_process.v);
    if (len < 0)
        return -EIO;
    if ((size_t)len >= sizeof(status))
        len = sizeof(status) - 1;
    size_t out = min(count, (size_t)len);
    if (either_copyout(user, (uint64)buf, status, out) < 0)
        return -EFAULT;
    hvdxg.read_emitted = 1;
    return (int)out;
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

static uint64 hvdxg_map_iospace_user(uint64 pa, uint64 size)
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

static int hvdxg_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    (void)cdev;
    int ret = -EINVAL;

    hvdxg.ioctl_count++;

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
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
        break;
    }

    case LX_DXDESTROYDEVICE: {
        struct d3dkmt_destroydevice req;
        struct hvdxg_command_destroydevice destroy;
        struct hvdxg_ntstatus status;
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
        memset(&destroy, 0, sizeof(destroy));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(&destroy.hdr,
                                        HV_DXGK_VMBCOMMAND_DESTROYDEVICE,
                                        hvdxg.dxg_process);
        destroy.device.v = req.device.v;
        ret = hvdxg_send_sync_vgpu(&destroy, sizeof(destroy), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
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
            result.fence_storage_physical_address, PGSIZE);
        if (req.fence_cpu_virtual_address == 0) {
            ret = -ENOMEM;
            break;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
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
        if (req.adapter.v != hvdxg.host_adapter_handle || req.size == 0) {
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
        break;
    }

    case LX_DXFREEGPUVIRTUALADDRESS: {
        struct d3dkmt_freegpuvirtualaddress req;
        struct hvdxg_command_freegpuvirtualaddress free_gpuva;
        struct hvdxg_ntstatus status;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapter.v != hvdxg.host_adapter_handle ||
            req.base_address == 0 || req.size == 0) {
            ret = -EINVAL;
            break;
        }
        memset(&free_gpuva, 0, sizeof(free_gpuva));
        memset(&status, 0, sizeof(status));
        hvdxg_command_vgpu_init_process(
            &free_gpuva.hdr, HV_DXGK_VMBCOMMAND_FREEGPUVIRTUALADDRESS,
            hvdxg.dxg_process);
        free_gpuva.args = req;
        ret = hvdxg_send_sync_vgpu(&free_gpuva, sizeof(free_gpuva), &status,
                                   sizeof(status), &actual_len);
        if (ret == 0 && actual_len >= sizeof(status))
            ret = hvdxg_ntstatus_to_errno(status);
        hvdxg.gpuva_free_last_len = actual_len;
        hvdxg.gpuva_free_last_ret = ret;
        break;
    }

    case LX_DXCREATESYNCHRONIZATIONOBJECT: {
        struct d3dkmt_createsynchronizationobject2 req;
        struct hvdxg_command_createsyncobject create;
        struct hvdxg_command_createsyncobject_return result;
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
            req.info.monitored_fence.fence_cpu_virtual_address = 0;
            req.info.monitored_fence.fence_gpu_virtual_address =
                result.fence_gpu_va;
        } else if (req.info.type == _D3DDDI_PERIODIC_MONITORED_FENCE) {
            req.info.periodic_monitored_fence.fence_cpu_virtual_address = 0;
            req.info.periodic_monitored_fence.fence_gpu_virtual_address =
                result.fence_gpu_va;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
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
        break;
    }

    case LX_DXCREATECONTEXTVIRTUAL: {
        struct d3dkmt_createcontextvirtual req;
        uint8 command_buf[sizeof(struct hvdxg_command_createcontextvirtual) +
                          HV_DXG_IOCTL_PRIVATE_MAX - 1];
        struct hvdxg_command_createcontextvirtual *create =
            (struct hvdxg_command_createcontextvirtual *)command_buf;
        uint32 command_len;
        uint32 actual_len = 0;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.device.v == 0 ||
            req.priv_drv_data_size > HV_DXG_IOCTL_PRIVATE_MAX ||
            (req.priv_drv_data_size != 0 && req.priv_drv_data == 0)) {
            ret = -EINVAL;
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        hvdxg_command_vgpu_init_process(&create->hdr,
                                        HV_DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL,
                                        hvdxg.dxg_process);
        create->device.v = req.device.v;
        create->node_ordinal = req.node_ordinal;
        create->engine_affinity = req.engine_affinity;
        create->flags = req.flags;
        create->client_hint = (uint32)req.client_hint;
        create->priv_drv_data_size = req.priv_drv_data_size;
        if (req.priv_drv_data_size != 0 &&
            either_copyin(create->priv_drv_data, 1, req.priv_drv_data,
                          req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            break;
        }
        command_len = sizeof(*create) + req.priv_drv_data_size - 1;
        ret = hvdxg_send_sync_vgpu(create, command_len, command_buf,
                                   command_len, &actual_len);
        if (ret != 0)
            break;
        if (actual_len < sizeof(*create) - 1 || create->context.v == 0) {
            ret = -EIO;
            break;
        }
        req.context.v = create->context.v;
        if (req.priv_drv_data_size != 0 &&
            actual_len >= sizeof(*create) + req.priv_drv_data_size - 1 &&
            either_copyout(1, req.priv_drv_data, create->priv_drv_data,
                           req.priv_drv_data_size) < 0) {
            ret = -EFAULT;
            break;
        }
        ret = either_copyout(1, (uint64)arg, &req, sizeof(req)) < 0 ?
              -EFAULT : 0;
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
        struct d3dkmt_queryadapterinfo req;
        uint8 command_buf[sizeof(struct hvdxg_command_queryadapterinfo) +
                          HV_DXG_IOCTL_PRIVATE_MAX - 1];
        uint8 result_buf[sizeof(struct hvdxg_ntstatus) +
                         HV_DXG_IOCTL_PRIVATE_MAX];
        struct hvdxg_command_queryadapterinfo *query =
            (struct hvdxg_command_queryadapterinfo *)command_buf;
        uint32 command_len;
        uint32 actual_len = 0;
        uint8 *private_data = result_buf;

        ret = hvdxg_d3dkmt_ensure();
        if (ret != 0)
            break;
        if (either_copyin(&req, 1, (uint64)arg, sizeof(req)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (req.adapter.v != hvdxg.host_adapter_handle ||
            req.private_data == 0 || req.private_data_size == 0 ||
            req.private_data_size > HV_DXG_IOCTL_PRIVATE_MAX) {
            ret = -EINVAL;
            break;
        }
        memset(command_buf, 0, sizeof(command_buf));
        hvdxg_command_vgpu_init_process(&query->hdr,
                                        HV_DXGK_VMBCOMMAND_QUERYADAPTERINFO,
                                        hvdxg.dxg_process);
        query->query_type = (uint32)req.type;
        query->private_data_size = req.private_data_size;
        if (either_copyin(query->private_data, 1, req.private_data,
                          req.private_data_size) < 0) {
            ret = -EFAULT;
            break;
        }
        command_len = sizeof(*query) + req.private_data_size - 1;
        memset(result_buf, 0, sizeof(result_buf));
        ret = hvdxg_send_sync_vgpu(query, command_len, result_buf,
                                   req.private_data_size +
                                   sizeof(struct hvdxg_ntstatus),
                                   &actual_len);
        if (ret != 0)
            break;
        if (actual_len >= req.private_data_size +
                         sizeof(struct hvdxg_ntstatus)) {
            struct hvdxg_ntstatus *status =
                (struct hvdxg_ntstatus *)result_buf;
            ret = hvdxg_ntstatus_to_errno(*status);
            if (ret < 0)
                break;
            private_data = result_buf + sizeof(struct hvdxg_ntstatus);
        } else if (actual_len < req.private_data_size) {
            ret = -EOVERFLOW;
            break;
        }
        if ((req.type == _KMTQAITYPE_ADAPTERTYPE ||
             req.type == _KMTQAITYPE_ADAPTERTYPE_RENDER) &&
            req.private_data_size >= sizeof(struct d3dkmt_adaptertype)) {
            struct d3dkmt_adaptertype *adapter_type =
                (struct d3dkmt_adaptertype *)private_data;
            adapter_type->paravirtualized = 1;
            adapter_type->display_supported = 0;
            adapter_type->post_device = 0;
            adapter_type->indirect_display_device = 0;
            adapter_type->acg_supported = 0;
            adapter_type->support_set_timings_from_vidpn = 0;
        }
        ret = either_copyout(1, req.private_data, private_data,
                             req.private_data_size) < 0 ? -EFAULT : 0;
        break;
    }

    default:
        ret = -ENOTSUP;
        break;
    }

    hvdxg.ioctl_last_ret = ret;
    if (ret >= 0)
        hvdxg.ioctl_successes++;
    return ret;
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
    return hvdxg.global_open_ok && hvdxg.vgpu_open_ok;
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

static void hvdxg_process_channel_packets(struct hv_ring_buffer *in_ring,
                                          uint32 *counter)
{
    uint8 pkt[1200];
    uint32 len;
    uint16 type;

    while (in_ring && hv_recv_raw_on(in_ring, pkt, sizeof(pkt), &len,
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
        if (type == VM_PKT_COMP || type == VM_PKT_DATA_INBAND)
            hvdxg_capture_completion(payload, payload_len, type,
                                     desc->trans_id);
    }
}

static int hvdxg_wait_completion(uint64 trans_id, void *out,
                                 uint32 out_len, uint32 *actual_len,
                                 uint64 timeout_ms)
{
    for (uint64 i = 0; i < timeout_ms; i++) {
        hv_process_messages();
        hv_process_events();
        if (hvdxg.vgpu_open_ok)
            hvdxg_process_channel_packets(hvdxg.vgpu_in_ring,
                                          &hvdxg.vgpu_rx_packets);
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
    uint64 trans_id;
    int ret;

    if (!hvdxg.vgpu_open_ok || hvdxg.vgpu_out_ring == NULL)
        return -ENODEV;

    trans_id = hvdxg_next_trans_id();
    __atomic_store_n(&hvdxg.completion_pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&hvdxg.waiting_trans_id, trans_id, __ATOMIC_RELEASE);
    ret = hv_send_packet_on(hvdxg.vgpu_out_ring, hvdxg.vgpu_relid,
                            hvdxg.vgpu_conn_id,
                            hvdxg.vgpu_monitor_allocated,
                            hvdxg.vgpu_monitorid, hvdxg.vgpu_dedicated,
                            cmd, cmd_len, trans_id,
                            VM_PKT_COMPLETION_REQUESTED);
    if (ret != 0) {
        __atomic_store_n(&hvdxg.waiting_trans_id, 0, __ATOMIC_RELEASE);
        return ret;
    }
    return hvdxg_wait_completion(trans_id, result, result_len, actual_len,
                                 HV_DXG_WAIT_MS);
}

static int hvdxg_send_sync_global(const void *cmd, uint32 cmd_len,
                                  void *result, uint32 result_len,
                                  uint32 *actual_len)
{
    uint64 trans_id;
    int ret;

    if (!hvdxg.global_open_ok || hvdxg.global_out_ring == NULL)
        return -ENODEV;

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
        return ret;
    }
    return hvdxg_wait_completion(trans_id, result, result_len, actual_len,
                                 HV_DXG_WAIT_MS);
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

    memset(&open, 0, sizeof(open));
    hvdxg_command_vgpu_init(&open.hdr, HV_DXGK_VMBCOMMAND_OPENADAPTER);
    open.vmbus_interface_version = HV_DXG_VMBUS_INTERFACE_VERSION_OLD;
    open.vmbus_last_compatible_interface_version =
        HV_DXG_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;
    open.guest_adapter_luid = hvdxg_luid_from_guid(&hvdxg.vgpu_instance);

    memset(&open_ret, 0, sizeof(open_ret));
    ret = hvdxg_send_sync_vgpu(&open, sizeof(open), &open_ret,
                               sizeof(open_ret), &actual_len);
    hvdxg.probe_last_ret = ret;
    if (ret != 0)
        return ret;
    if (actual_len < sizeof(open_ret)) {
        hvdxg.probe_last_ret = -EOVERFLOW;
        return -EOVERFLOW;
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
        return -EIO;
    }

    memset(&info, 0, sizeof(info));
    hvdxg_command_vgpu_init(&info.hdr,
                            HV_DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO);
    memset(&info_ret, 0, sizeof(info_ret));
    actual_len = 0;
    ret = hvdxg_send_sync_vgpu(&info, sizeof(info), &info_ret,
                               sizeof(info_ret) - sizeof(struct hvdxg_winluid),
                               &actual_len);
    hvdxg.probe_last_ret = ret;
    if (ret != 0)
        return ret;
    hvdxg.probe_info_len = actual_len;
    if (actual_len >= sizeof(uint32) * 4) {
        hvdxg.probe_info_flags = info_ret.flags;
        hvdxg.probe_async_msg_enabled = (info_ret.flags >> 6) & 1U;
        hvdxg.host_adapter_luid = info_ret.host_adapter_luid;
        hvdxg.host_vgpu_luid = info_ret.host_vgpu_luid;
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
    if (hvdxg.global_open_ok && hvdxg.global_relid != 0 &&
        hv_test_and_clear_event(hvdxg.global_relid))
        hvdxg_process_channel_packets(hvdxg.global_in_ring,
                                      &hvdxg.global_rx_packets);
    if (hvdxg.vgpu_open_ok && hvdxg.vgpu_relid != 0 &&
        hv_test_and_clear_event(hvdxg.vgpu_relid))
        hvdxg_process_channel_packets(hvdxg.vgpu_in_ring,
                                      &hvdxg.vgpu_rx_packets);
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
