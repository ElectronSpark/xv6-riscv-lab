/*
 * Hyper-V implementation root.
 *
 * This unity-style root keeps the current static helper relationships intact
 * while the implementation is split by subsystem under dev/hyperv/.
 */

#include "hyperv_common.c"

#if defined(__x86_64__) || defined(__i386__)
#include "hyperv_defs_state.c"
#include "hyperv_vpci_config.c"
#include "hyperv_dxg_state.c"
#include "hyperv_dxg_pci_version.c"
#include "hyperv_dxg_queryadapter_hwid.c"
#include "hyperv_dxg_diag.c"
#include "hyperv_dxg_status_device.c"
#include "hyperv_dxg_memory.c"
#include "hyperv_dxg_handle_manager.c"
#include "hyperv_dxg_allocations.c"
#include "hyperv_dxg_shared_objects.c"
#include "hyperv_dxg_ioctl_queryadapter.c"
#include "hyperv_dxg_ioctls.c"
#include "hyperv_dxg_device.c"
#include "hyperv_vmbus_core.c"
#include "hyperv_synth_devices.c"
#include "hyperv_init_public.c"
#else
#include "hyperv_non_x86.c"
#endif
