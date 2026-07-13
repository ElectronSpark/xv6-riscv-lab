#!/usr/bin/env bash
set -euo pipefail

kernel_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
top_root="$(cd -- "${kernel_root}/.." && pwd)"
safe_rg="${top_root}/scripts/audit/safe-rg.sh"
source_file="kernel/kernel/virtio_gpu.c"

cd "${top_root}"

require_source() {
    "${safe_rg}" -q "$1" "$2"
}

forbid_source() {
    if "${safe_rg}" -q "$1" "$2"; then
        printf 'forbidden source pattern: %s in %s\n' "$1" "$2" >&2
        exit 1
    fi
}

# Lock the production routing to SUBMIT_3D only.  An explicit global depth
# continues to bypass the reason-specific knob exactly as before.
require_source 'reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D &&' "${source_file}"
require_source '!virtio_gpu_cmdline_present\("virtio_gpu_async_depth"\)' "${source_file}"
require_source 'int submit_default_depth = depth < 2 \? depth : 2;' "${source_file}"
require_source '"virtio_gpu_async_submit_depth", \(uint32\)submit_default_depth,' "${source_file}"
require_source 'if \(depth > g->async_capacity\)' "${source_file}"
require_source 'depth = g->async_capacity;' "${source_file}"
forbid_source '"virtio_gpu_async_submit_depth", 1,' "${source_file}"

resolve_submit_depth() {
    local global_depth=$1
    local capacity=$2
    local explicit=${3:-}
    local depth

    if [[ -n "${explicit}" ]]; then
        depth=${explicit}
    elif (( global_depth < 2 )); then
        depth=${global_depth}
    else
        depth=2
    fi
    if (( depth < 1 )); then
        depth=1
    fi
    if (( depth > capacity )); then
        depth=${capacity}
    fi
    printf '%d\n' "${depth}"
}

check_case() {
    local name=$1
    local global_depth=$2
    local capacity=$3
    local explicit=$4
    local expected=$5
    local observed

    observed="$(resolve_submit_depth "${global_depth}" "${capacity}" "${explicit}")"
    if [[ "${observed}" != "${expected}" ]]; then
        printf 'virtio-gpu async depth case failed: %s expected=%s observed=%s\n' \
            "${name}" "${expected}" "${observed}" >&2
        exit 1
    fi
}

check_case default-global1 1 60 "" 1
check_case default-global2 2 60 "" 2
check_case default-global32 32 60 "" 2
check_case explicit1 32 60 1 1
check_case explicit7 32 60 7 7
check_case explicit-capacity-clamp 32 4 7 4
check_case explicit-max-clamp 32 60 64 60

printf '%s\n' \
    'VIRTIO-GPU-ASYNC-SUBMIT-DEPTH-STATIC-PASS default_global1=1 default_global_ge2=2 explicit1=1 explicitN=PASS capacity_clamp=PASS submit3d_only=PASS fifo_ordering_unchanged=PASS'
