
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
#define HV_PCI_GPADL_HANDLE 0xE1E17
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
#define HV_DXG_SEND_EAGAIN_RETRIES 50
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
#define HV_DXGK_VMBCOMMAND_MARKDEVICEASERROR 29U
#define HV_DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA 39U
#define HV_DXGK_VMBCOMMAND_OPENRESOURCE 32U
#define HV_DXGK_VMBCOMMAND_SETCONTEXTSCHEDULINGPRIORITY 33U
#define HV_DXGK_VMBCOMMAND_FLUSHHEAPTRANSITIONS 37U
#define HV_DXGK_VMBCOMMAND_QUERYALLOCATIONRESIDENCY 41U
#define HV_DXGK_VMBCOMMAND_FLUSHDEVICE 42U
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
#define HV_DXG_FLUSHSCHEDULER_DEVICE_TERMINATE 4U
#define HV_DXG_PCI_VMBUS_VERSION_OFFSET 208U
#define HV_DXG_PCI_GUESTCAPS_OFFSET 212U
#define HV_DXG_PCI_GUESTCAPS_WSL2 1U
#define HV_DXG_IOCTL_PRIVATE_MAX 1024U
#define HV_DXG_ALLOCATION_MAX 8U
#define HV_DXG_RESOURCE_TRACKED_MAX 64U
#define HV_DXG_RESIDENCY_ALLOCATION_MAX (1024U * 10U)
#define HV_DXG_SYSMEM_PFNS_PER_PACKET 128U
#define HV_DXG_GPUVA_UPDATE_MAX 16U
#define HV_DXG_HISTORY_BUFFER_MAX 64U
#define HV_DXG_OPEN_TRACKED_MAX 512U
#define HV_DXG_OBJECT_TABLE_MAX 8192U
#define HV_DXG_PROCESS_TABLE_MAX 64U
#define HV_DXG_HOST_EVENT_MAX 64U
#define HV_DXG_HOST_EVENT_TIMEOUT_MS 65000U
#define HV_DXG_SHARE_RESIDENCY_WAIT_MS 10000U
#define HV_DXG_STATUS_BUF_SIZE (64U * 1024U)
#define HV_DXG_QUERY_HISTORY_MAX 16U
#define HV_DXG_IOCTL_HISTORY_MAX 16U
#define HV_DXG_IOCTL_NR_MAX 256U
#define HV_DXG_IOCTL_TIME_TOP 4U
#define HV_DXG_RESOURCE_HISTORY_MAX 8U
#define HV_DXG_STATUS_PENDING 0x00000103U
#define HV_DXG_SHARED_ALLOC_HEAD_MAX 1024U
#define HV_DXG_CONTEXT_PRIV_HEAD_MAX 4096U
#define HV_DXG_HWQUEUE_PRIV_HEAD_MAX 1024U
#define HV_DXG_HWQUEUE_SUBMIT_PRIV_HEAD_MAX 512U
#define HV_DXG_NTSHARED_ATTEMPT_MAX 5U
#define HV_DXG_NTSHARED_RAW_BYTES 48U
#define HV_DXG_NTSHARED_CACHE_MAX 64U
#define HV_DXG_NTSHARED_LABEL_WSL_EXT24_ZERO_LUID 1U
#define HV_DXG_NTSHARED_LABEL_WSL_NOEXT24 2U
#define HV_DXG_NTSHARED_LABEL_EXT24_ZERO_LUID 3U
#define HV_DXG_NTSHARED_LABEL_EXT24_HOST_LUID 4U
#define HV_DXG_NTSHARED_LABEL_NOEXT28 5U
#define HV_DXG_NTSHARED_LABEL_NOEXT24 6U
#define HV_DXG_NTSHARED_LABEL_NOEXT32_FALLBACK 7U
#define HV_DXG_NTSHARED_LABEL_GLOBAL_NOEXT32 8U
#define HV_DXG_NTSHARED_LABEL_GLOBAL_EXT32_ZERO_LUID 9U
#define HV_DXG_NTSHARED_LABEL_LEGACY_EXT32_ZERO_LUID 10U
#define HV_DXG_NTSHARED_LABEL_WSL_NOEXT32_NATURAL 11U
#define HV_DXG_NTSHARED_LABEL_WSL_EXT32_ZERO_LUID_NATURAL 12U
#define HV_DXG_DESTROY_ALLOC_CTX_HELPER 1U
#define HV_DXG_DESTROY_ALLOC_CTX_DEVICE_RESOURCE 2U
#define HV_DXG_DESTROY_ALLOC_CTX_DEVICE_STANDALONE 3U
#define HV_DXG_DESTROY_ALLOC_CTX_FILE_CLEANUP 4U
#define HV_DXG_DESTROY_ALLOC_CTX_CREATE_UNWIND 5U
#define HV_DXG_DESTROY_ALLOC_CTX_IOCTL 6U
#define HV_DXG_ALLOCATION_FLAG_CACHED (1U << 19)
#define HV_DXG_SYNC_SPIN_POLLS 4096U
#define HV_DXG_QAITYPE_SELECTED_ADAPTER 0U
#define HV_DXG_QAITYPE_ADAPTER_HARDWARE_ID 31U
#define HV_DXG_QAITYPE_CHECKDRIVERUPDATESTATUS_RENDER 55U
#define HV_DXG_USER_LUID_SOURCE_NONE 0U
#define HV_DXG_USER_LUID_SOURCE_ADAPTER 1U
#define HV_DXG_USER_LUID_SOURCE_HOST_VGPU 2U
#define HV_DXG_USER_LUID_SOURCE_PCI_HOST_VGPU 3U
#define HV_DXG_USER_LUID_SOURCE_GUID 4U
#define HV_DXG_OAFLUID_REJECT_NONE 0U
#define HV_DXG_OAFLUID_REJECT_ZERO 1U
#define HV_DXG_OAFLUID_REJECT_MISMATCH 2U
#define HV_DXG_QUERYADAPTERINFO_WSL_NATURAL_BASE 33U
#define HV_DXG_QUERYADAPTER_EXT_SNAPSHOT_BYTES 16U
#define HV_DXG_QUERYADAPTER_CMDHDR_SNAPSHOT_BYTES 24U
#define HV_DXG_QUERYADAPTER_PRIV_SNAPSHOT_BYTES 32U
#define HV_DXG_QUERYADAPTER_TYPE0_SNAPSHOT_BYTES 32U
#define HV_DXG_QUERYADAPTER_PAYLOAD_HEAD_BYTES 32U
#define HV_DXG_QAI_ADMISSION_HISTORY_MAX 8U
#define HV_DXG_QUERYADAPTER_ALIAS_CACHE_TYPES 5U
#define HV_DXG_QUERYADAPTER_ALIAS_CACHE_MAX 512U
#define HV_DXG_QUERYADAPTER_TYPE0_CACHE_MAX 65536U
#define HV_DXG_PHYSICAL_ADAPTER_DEVICE_IDS_SIZE (7U * sizeof(uint32))
#define HV_DXG_SEND_SYNC_VGPU_F_NO_EXT_HEADER 0x1U
#define HV_DXG_SEND_SYNC_VGPU_F_FORCE_VGPU    0x2U
#define HV_DXG_CHANNEL_NONE   0U
#define HV_DXG_CHANNEL_GLOBAL 1U
#define HV_DXG_CHANNEL_VGPU   2U
#define HV_DXG_QAI_ADMISSION_KIND_QAI 1U
#define HV_DXG_QAI_ADMISSION_KIND_CREATEDEVICE 2U
#define HV_DXG_QAITYPE_CREATEDEVICE_MARKER 0xffffffffU
#define HV_DXG_QAITYPE_PHASE1_TYPE27 27U
#define HV_DXG_VENDOR_INTEL 0x8086U
#define HV_DXG_VENDOR_NVIDIA 0x10deU
/*
 * libd3d12core maps WSL NVIDIA DriverStore names to
 * /usr/lib/wsl/drivers/<FileRepository-dir>/libnvwgf2umx.so. This default
 * matches the xv6 Mesa staging layout when kernel-side dynamic detection is
 * unavailable.
 */
#define HV_DXG_NVIDIA_UMD_DRIVERSTORE_PATH \
    "C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nvmi.inf_amd64_9a9d1548c06ce277\\libnvwgf2umx.so"
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

static const struct hv_guid hv_pci_guid = {
    0x44c4f61d, 0x4444, 0x4400,
    { 0x9d, 0x52, 0x80, 0x2e, 0x27, 0xed, 0xe1, 0x9f }
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

#define HVPCI_MAKE_VERSION(maj, min) ((((uint32)(maj)) << 16) | (min))
#define HVPCI_PROTOCOL_VERSION_1_1 HVPCI_MAKE_VERSION(1, 1)
#define HVPCI_PROTOCOL_VERSION_1_2 HVPCI_MAKE_VERSION(1, 2)
#define HVPCI_PROTOCOL_VERSION_1_3 HVPCI_MAKE_VERSION(1, 3)
#define HVPCI_PROTOCOL_VERSION_1_4 HVPCI_MAKE_VERSION(1, 4)
#define HVPCI_MESSAGE_BASE 0x42490000U
#define HVPCI_BUS_RELATIONS (HVPCI_MESSAGE_BASE + 0U)
#define HVPCI_QUERY_BUS_RELATIONS (HVPCI_MESSAGE_BASE + 1U)
#define HVPCI_BUS_D0ENTRY (HVPCI_MESSAGE_BASE + 7U)
#define HVPCI_QUERY_PROTOCOL_VERSION (HVPCI_MESSAGE_BASE + 0x13U)
#define HVPCI_BUS_RELATIONS2 (HVPCI_MESSAGE_BASE + 0x19U)
#define HVPCI_STATUS_REVISION_MISMATCH 0xC0000059U
#define HVPCI_CHILD_MAX 16U
#define HVPCI_CONFIG_MMIO_LENGTH 0x2000U
#define HVPCI_CONFIG_PAGE_OFFSET 0x1000U
#define HVPCI_STATIC_MMIO_BASE 0xFE000000ULL
#define HVPCI_STATIC_MMIO_END  0xFEC00000ULL

struct hvpci_version_request {
    uint32 type;
    uint32 protocol_version;
} __PACKED;

struct hvpci_message {
    uint32 type;
} __PACKED;

struct hvpci_bus_d0_entry {
    struct hvpci_message message_type;
    uint32 reserved;
    uint64 mmio_base;
} __PACKED;

struct hvpci_function_description {
    uint16 vendor_id;
    uint16 device_id;
    uint8 revision_id;
    uint8 prog_intf;
    uint8 subclass;
    uint8 base_class;
    uint32 subsystem_id;
    uint32 win_slot;
    uint32 serial;
} __PACKED;

struct hvpci_function_description2 {
    struct hvpci_function_description base;
    uint32 flags;
    uint16 virtual_numa_node;
    uint16 reserved;
} __PACKED;

struct hvpci_response {
    struct vmpacket_descriptor hdr;
    int32 status;
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

struct hvdxg_command_vgpu_to_host_wsl {
    uint64 command_id;
    struct hvdxg_d3dkmthandle process;
    uint32 channel_type;
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

struct hvdxg_command_createprocess_wsl {
    struct hvdxg_command_vm_to_host hdr;
    uint32 hdr_tail_padding;
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

struct hvdxg_command_destroyprocess_wsl {
    struct hvdxg_command_vm_to_host hdr;
    uint32 hdr_tail_padding;
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

struct hvdxg_command_queryadapterinfo_wsl {
    struct hvdxg_command_vgpu_to_host_wsl hdr;
    uint32 query_type;
    uint32 private_data_size;
    uint8 private_data[1];
    uint8 alignment_padding[7];
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
    struct hvdxg_d3dkmthandle object;
} __PACKED;

struct hvdxg_command_createntsharedobject_28 {
    struct hvdxg_command_vm_to_host hdr;
    uint32 hdr_tail_padding;
    struct hvdxg_d3dkmthandle object;
} __PACKED;

struct hvdxg_command_createntsharedobject_wsl24 {
    struct hvdxg_command_vm_to_host hdr;
    struct hvdxg_d3dkmthandle object;
} __PACKED;

struct hvdxg_command_createntsharedobject_32 {
    struct hvdxg_command_vm_to_host hdr;
    uint32 hdr_tail_padding;
    struct hvdxg_d3dkmthandle object;
    uint32 command_tail_padding;
} __PACKED;

struct hvdxg_command_destroyntsharedobject {
    struct hvdxg_command_vm_to_host hdr;
    uint32 hdr_tail_padding;
    struct hvdxg_d3dkmthandle shared_handle;
    uint32 command_tail_padding;
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

struct hvdxg_command_flushdevice {
    struct hvdxg_command_vgpu_to_host hdr;
    struct hvdxg_d3dkmthandle device;
    uint32 reason;
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
    uint8 command_tail_padding[7];
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
    uint32 command_tail_padding;
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

struct hvdxg_command_markdeviceaserror {
    struct hvdxg_command_vgpu_to_host hdr;
    struct d3dkmt_markdeviceaserror args;
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
    uint32 alignment_padding;
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
    uint32 operations_padding;
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
    uint32 hdr_tail_padding;
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
    uint32 hdr_tail_padding;
    struct hvdxg_d3dkmthandle sync_object;
    uint32 command_tail_padding;
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
    uint32 owner_process;
    uint32 owner_generation;
    uint32 owner_refs;
    uint32 flags;
    uint32 lock_refcount;
    uint32 sysmem_page_count;
    uint64 size;
    uint64 cpu_va;
    uint64 map_size;
    uint64 sysmem;
    uint64 *sysmem_pages;
    void *cpu_vm;
    uint32 map_paging_queue;
    uint32 map_sync_object;
    int32 map_ret;
    int32 map_status;
    uint64 map_gpu_va;
    uint64 map_pages;
    uint64 map_fence_value;
    uint32 resident_paging_queue;
    uint32 resident_sync_object;
    uint64 resident_fence_value;
    uint32 resident_wait_result;
    int32 resident_wait_ret;
    uint64 resident_wait_current;
};

struct hvdxg_shared_resource_record {
    uint32 create_flags_value;
    uint32 host_create_flags_value;
    uint32 private_runtime_data_size;
    uint32 resource_priv_drv_data_size;
    uint32 total_priv_drv_data_size;
    uint32 sealed_generation;
    uint8 create_shared;
    uint8 nt_security_sharing;
    uint8 sealed;
    uint8 opened_from_shared;
    uint8 total_priv_from_host;
};

struct hvdxg_shared_allocation_record {
    uint32 allocation;
    uint32 priv_drv_data_size;
    uint64 size;
    uint32 flags;
};

struct hvdxg_tracked_resource {
    uint32 device;
    uint32 resource;
    uint32 global_share;
    uint32 owner_process;
    uint32 owner_generation;
    uint32 owner_refs;
    uint32 allocation_count;
    uint32 create_flags_value;
    uint32 host_create_flags_value;
    uint32 runtime_d3d12_flags;
    uint8 create_shared;
    uint8 nt_security_sharing;
    uint8 sealed;
    uint8 opened_from_shared;
    uint8 shared_metadata_created;
    uint32 host_shared_handle_nt;
    uint32 host_shared_process;
    uint32 host_shared_object;
    uint32 host_shared_refs;
    uint32 host_shared_sealed;
    uint32 sealed_generation;
    uint32 open_count;
    uint32 private_runtime_data_size;
    uint32 resource_priv_drv_data_size;
    uint32 total_priv_drv_data_size;
    uint8 total_priv_from_host;
    uint8 existing_sysmem;
    uint8 existing_sysmem_vram;
    uint32 existing_sysmem_pfnmap_pages;
    uint64 existing_sysmem_va;
    uint64 existing_sysmem_size;
    uint8 *private_runtime_data;
    uint8 *resource_priv_drv_data;
    uint8 *total_priv_drv_data;
    uint32 allocation_handles[HV_DXG_ALLOCATION_MAX];
    uint32 alloc_priv_sizes[HV_DXG_ALLOCATION_MAX];
    uint64 allocation_sizes[HV_DXG_ALLOCATION_MAX];
    uint32 allocation_flags[HV_DXG_ALLOCATION_MAX];
    uint8 shared_records_valid;
    struct hvdxg_shared_resource_record shared_resource_record;
    struct hvdxg_shared_allocation_record
        shared_allocation_records[HV_DXG_ALLOCATION_MAX];
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
#define HV_DXG_SHARED_OBJECT_KIND_MAX 3

enum hvdxg_shared_object_fops_kind {
    HV_DXG_SHARED_FOPS_NONE = 0,
    HV_DXG_SHARED_FOPS_SYNC = 1,
    HV_DXG_SHARED_FOPS_RESOURCE = 2,
};

struct hvdxg_shared_object {
    uint32 kind;
    uint32 device;
    uint32 object;
    uint32 global_share;
    uint32 host_nt_handle;
    uint32 cache_process;
    uint32 cache_object;
    uint64 nt_handle;
    uint32 sync_type;
    uint32 sync_flags;
    uint32 sync_owner_process;
    uint32 sync_owner_generation;
    uint32 sync_owner_refs;
    struct hvdxg_tracked_resource resource;
};

#define HV_DXG_DEFERRED_SHARED_DESTROY_MAX 16

struct hvdxg_deferred_shared_destroy {
    uint32 valid;
    uint32 process;
    uint32 object;
    uint32 nt_handle;
    uint32 device;
    uint32 resource;
};

struct hvdxg_sync_file_object {
    uint32 device;
    uint32 sync_object;
    uint32 global_share;
    uint32 host_nt_handle;
    uint32 cache_process;
    uint32 cache_object;
    uint32 sync_type;
    uint32 sync_flags;
    uint64 fence_value;
    uint64 event_id;
};

struct hvdxg_ntshared_cache_entry {
    uint32 kind;
    uint32 process;
    uint32 object;
    uint32 host_nt_handle;
    uint32 refs;
};

struct hvdxg_queryadapter_alias_cache_entry {
    uint32 valid;
    uint32 type;
    uint32 size;
    uint32 host_adapter;
    uint32 raw_hash;
    uint8 payload[HV_DXG_QUERYADAPTER_ALIAS_CACHE_MAX];
};

struct hvdxg_global_send_diag {
    uint64 command_id;
    uint32 command;
    uint32 cmd_len;
    uint32 wire_len;
    uint32 result_len;
    uint32 ext;
    uint32 ext_offset;
    uint32 process;
    uint32 channel;
    uint32 relid;
    uint32 conn_id;
    uint32 monitor_allocated;
    uint32 monitorid;
    uint32 dedicated;
    struct hvdxg_winluid luid;
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
    uint32 context;
    uint32 device;
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
    uint32 owner_process;
    uint32 owner_generation;
    uint32 owner_refs;
    uint32 flags;
    uint32 global_shared;
    uint32 monitor_fence_handle;
    uint64 fence_cpu_va;
    uint64 fence_kva;
};

enum hvdxg_object_type {
    HV_DXG_OBJECT_NONE = 0,
    HV_DXG_OBJECT_ADAPTER,
    HV_DXG_OBJECT_DEVICE,
    HV_DXG_OBJECT_CONTEXT,
    HV_DXG_OBJECT_HWQUEUE,
    HV_DXG_OBJECT_PAGINGQUEUE,
    HV_DXG_OBJECT_SYNC,
    HV_DXG_OBJECT_ALLOCATION,
    HV_DXG_OBJECT_RESOURCE,
    HV_DXG_OBJECT_GPUVA,
};

struct hvdxg_object_entry {
    uint64 handle;
    uint64 host_handle;
    uint64 parent;
    uint32 type;
    uint32 device;
    uint32 refs;
    uint32 generation;
    uint32 index;
    uint32 unique;
    uint32 instance;
    uint32 destroyed;
    uint32 destroyed_serial;
    uint32 free_prev;
    uint32 free_next;
    uint8 on_free_list;
};

struct hvdxg_process_adapter {
    uint32 host_adapter_handle;
    struct hvdxg_winluid adapter_luid;
    struct hvdxg_winluid host_adapter_luid;
    struct hvdxg_winluid host_vgpu_luid;
    uint32 refs;
    uint32 local_handle_count;
    uint32 generation;
    uint32 destroyed;
    uint32 *devices;
    uint32 device_count;
    uint32 device_capacity;
};

struct hvdxg_local_adapter_entry {
    uint32 handle;
    uint32 adapter_index;
    uint32 unique;
    uint32 generation;
    uint32 destroyed;
    uint32 destroyed_serial;
};

struct hvdxg_process_state {
    uint64 tgid;
    uint64 pid;
    uint64 guest_process;
    struct hvdxg_d3dkmthandle host_process;
    struct hvdxg_object_entry *objects;
    struct hvdxg_process_adapter *adapters;
    struct hvdxg_local_adapter_entry *local_adapters;
    uint32 host_process_created;
    uint32 process_refs;
    uint32 process_mem_refs;
    uint32 generation;
    uint32 object_count;
    uint32 object_capacity;
    uint32 object_free_count;
    uint32 object_free_head;
    uint32 object_free_tail;
    uint32 next_object_generation;
    uint32 object_alloc_serial;
    uint32 object_destroy_serial;
    uint32 adapter_count;
    uint32 adapter_capacity;
    uint32 local_adapter_count;
    uint32 local_adapter_capacity;
    uint32 next_adapter_generation;
    uint32 next_local_adapter_generation;
    uint32 local_adapter_alloc_serial;
    uint32 local_adapter_destroy_serial;
};

struct hvdxg_open_state {
    size_t read_offset;
    int read_emitted;
    char *read_status;
    size_t read_status_len;
    struct hvdxg_d3dkmthandle dxg_process;
    uint64 dxg_process_guest;
    uint64 dxg_process_pid;
    uint32 dxg_process_created;
    struct hvdxg_process_state *process_state;
    struct hvdxg_object_entry *objects;
    uint32 *devices;
    uint32 *contexts;
    struct hvdxg_tracked_hwqueue *hwqueues;
    struct hvdxg_tracked_pagingqueue *paging_queues;
    struct hvdxg_tracked_sync *sync_objects;
    struct hvdxg_tracked_allocation *allocations;
    struct hvdxg_tracked_resource *resources;
    struct hvdxg_tracked_gpuva *gpuvas;
    uint32 object_count;
    uint32 object_capacity;
    uint32 object_free_count;
    uint32 object_free_head;
    uint32 object_free_tail;
    uint32 next_generation;
    uint32 object_alloc_serial;
    uint32 object_destroy_serial;
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

enum hvdxg_probe_v40_reject_reason {
    HV_DXG_PROBE_V40_REJECT_NONE = 0,
    HV_DXG_PROBE_V40_REJECT_NOT_ATTEMPTED = 1,
    HV_DXG_PROBE_V40_REJECT_OPEN_SEND = 2,
    HV_DXG_PROBE_V40_REJECT_OPEN_SHORT = 3,
    HV_DXG_PROBE_V40_REJECT_OPEN_STATUS = 4,
    HV_DXG_PROBE_V40_REJECT_OPEN_ZERO_HANDLE = 5,
    HV_DXG_PROBE_V40_REJECT_GETINTERNAL_SEND = 6,
    HV_DXG_PROBE_V40_REJECT_GETINTERNAL_SHORT = 7,
};

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
    int present;
    int gpadl_ok;
    int open_ok;
    int protocol_ok;
    int config_window_ok;
    int backend_index;
    int backend_registered;
    int monitor_allocated;
    int dedicated;
    uint32 child_relid;
    uint32 signal_conn_id;
    uint8 monitorid;
    uint32 offer_count;
    uint32 offer_flags;
    uint32 offer_mmio_megabytes;
    uint32 offer_user_def[4];
    struct hv_guid offer_instance;
    uint32 gpadl_status;
    uint32 open_status;
    uint64 ring_pa;
    uint8 *ring;
    struct hv_ring_buffer *out_ring;
    struct hv_ring_buffer *in_ring;
    uint32 protocol_attempts;
    uint32 protocol_selected_version;
    uint32 protocol_last_version;
    int32 protocol_last_status;
    int32 protocol_last_ret;
    uint16 protocol_last_packet_type;
    uint32 protocol_last_len;
    uint64 protocol_last_trans_id;
    uint8 protocol_last_prefix[8];
    volatile int protocol_pending;
    uint64 config_window_pa;
    uint64 config_window_va;
    uint32 config_window_size;
    int32 config_window_ret;
    uint32 d0_attempts;
    uint32 d0_sent;
    int32 d0_status;
    int32 d0_ret;
    uint16 d0_packet_type;
    uint32 d0_len;
    uint64 d0_trans_id;
    uint8 d0_prefix[8];
    volatile int d0_pending;
    uint32 query_attempts;
    uint32 query_sent;
    int32 query_ret;
    uint16 query_packet_type;
    uint32 query_len;
    uint64 query_trans_id;
    uint8 query_prefix[8];
    uint32 relations_seen;
    uint32 relations_len;
    uint32 relations_count;
    uint32 relations_parse_ok;
    uint32 relations_desc_size;
    uint32 relations_count_offset;
    uint32 relations_desc_offset;
    uint8 relations_prefix[8];
    uint32 child_count;
    uint32 registered_count;
    uint32 register_last_ret;
    uint32 config_read_count;
    uint32 config_write_count;
    uint32 config_last_token;
    uint32 config_last_offset;
    uint32 config_last_size;
    uint32 config_last_value;
    uint32 config_reject_count;
    int32 config_last_ret;
    uint32 config_window_source;
    uint32 config_window_rejects;
    uint64 config_window_candidate;
    uint64 config_window_limit;
    spinlock_t config_lock;
    struct {
        uint32 win_slot;
        uint16 vendor_id;
        uint16 device_id;
        uint32 class_code;
        uint8 revision_id;
        uint16 subsystem_vendor_id;
        uint16 subsystem_id;
        uint8 bus;
        uint8 dev;
        uint8 func;
        int registered;
    } child[HVPCI_CHILD_MAX];
} hvpci;
