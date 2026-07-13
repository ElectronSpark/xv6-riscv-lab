#!/usr/bin/env bash
set -euo pipefail

kernel_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
top_root="$(cd -- "${kernel_root}/.." && pwd)"
safe_rg="${top_root}/scripts/audit/safe-rg.sh"
source_file="kernel/kernel/virtio_gpu.c"
max_depth=60

cd "${top_root}"

require_multiline() {
    local name=$1
    local pattern=$2

    if ! "${safe_rg}" --multiline -q "${pattern}" "${source_file}"; then
        printf 'virtio-gpu async depth source lock failed: %s\n' "${name}" >&2
        exit 1
    fi
}

# Lock one contiguous production resolver.  This proves the reason-specific
# branch remains SUBMIT_3D-only, an explicit global depth bypasses it, the new
# default is min(2, resolved global depth), the parser receives its real max,
# and the capacity clamp still follows parsing.
resolver_pattern='static int virtio_gpu_async_depth_for_reason\(
    struct virtio_gpu \*g, enum virtio_gpu_async_reason reason\)
\{
    int depth = virtio_gpu_async_depth\(g\);

    if \(reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D &&
        !virtio_gpu_cmdline_present\("virtio_gpu_async_depth"\)\) \{
(?s:.*?)        int submit_default_depth = depth < 2 \? depth : 2;

        depth = \(int\)virtio_gpu_cmdline_uint\(
            "virtio_gpu_async_submit_depth", \(uint32\)submit_default_depth,
            VIRTIO_GPU_ASYNC_MAX_DEPTH\);
        if \(depth < 1\)
            depth = 1;
        if \(depth > g->async_capacity\)
            depth = g->async_capacity;
    \}
    return depth;
\}'
require_multiline resolver-contiguous "${resolver_pattern}"

# Lock the parser's max clamp itself, not merely the resolver's max argument.
# The resolver lock above then proves parser max-clamping occurs before the
# independent negotiated-capacity clamp.
parser_pattern='static uint32 virtio_gpu_cmdline_uint\(const char \*key, uint32 default_value,
                                      uint32 max_value\)
\{
    char buf\[16\];
    uint32 value = 0;

    if \(cmdline_get_param\(key, buf, sizeof\(buf\)\) != 0 \|\| buf\[0\] == .\\0.\)
        return default_value;
    for \(int i = 0; buf\[i\] != .\\0.; i\+\+\) \{
        if \(buf\[i\] < .0. \|\| buf\[i\] > .9.\)
            return default_value;
        value = value \* 10 \+ \(uint32\)\(buf\[i\] - .0.\);
        if \(value > max_value\)
            return max_value;
    \}
    return value;
\}'
require_multiline parser-max-contiguous "${parser_pattern}"

resolver_source="$("${safe_rg}" --multiline --only-matching --no-filename \
    "${resolver_pattern}" "${source_file}")"
parser_source="$("${safe_rg}" --multiline --only-matching --no-filename \
    "${parser_pattern}" "${source_file}")"
mutation_file="kernel/tests/.virtio_gpu_async_depth_mutation.${BASHPID}"
if [[ -e ${mutation_file} || -L ${mutation_file} ]]; then
    printf 'virtio-gpu async depth mutation path already exists: %s\n' \
        "${mutation_file}" >&2
    exit 1
fi
trap 'rm -f -- "${mutation_file}"' EXIT

assert_mutation_rejected() {
    local name=$1
    local pattern=$2
    local original=$3
    local mutated=$4

    if [[ ${mutated} == "${original}" ]]; then
        printf 'virtio-gpu async depth mutation was not applied: %s\n' \
            "${name}" >&2
        exit 1
    fi
    printf '%s\n' "${mutated}" > "${mutation_file}"
    if "${safe_rg}" --multiline -q "${pattern}" "${mutation_file}"; then
        printf 'virtio-gpu async depth source mutation escaped: %s\n' \
            "${name}" >&2
        exit 1
    fi
}

mutated=${resolver_source/'int submit_default_depth = depth < 2 ? depth : 2;'/'int submit_default_depth = 1;'}
assert_mutation_rejected old-hardcoded-default1 "${resolver_pattern}" \
    "${resolver_source}" "${mutated}"

mutated=${resolver_source/'reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D &&'/'(reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D || reason == VIRTIO_GPU_ASYNC_REASON_FLUSH) &&'}
assert_mutation_rejected broadened-reason-branch "${resolver_pattern}" \
    "${resolver_source}" "${mutated}"

mutated=${resolver_source/'if (reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D &&
        !virtio_gpu_cmdline_present("virtio_gpu_async_depth")) {'/'if (!virtio_gpu_cmdline_present("virtio_gpu_async_depth") &&
        reason == VIRTIO_GPU_ASYNC_REASON_SUBMIT_3D) {'}
assert_mutation_rejected reordered-global-bypass "${resolver_pattern}" \
    "${resolver_source}" "${mutated}"

mutated=${parser_source/'        if (value > max_value)
            return max_value;
'/}
assert_mutation_rejected missing-parser-max-clamp "${parser_pattern}" \
    "${parser_source}" "${mutated}"

mutated=${resolver_source/'        if (depth > g->async_capacity)
            depth = g->async_capacity;
'/}
assert_mutation_rejected missing-capacity-clamp "${resolver_pattern}" \
    "${resolver_source}" "${mutated}"

parse_cmdline_uint() {
    local raw=$1
    local default_value=$2
    local max_value=$3
    local value=0
    local digit
    local char

    if [[ -z "${raw}" ]]; then
        printf '%d\n' "${default_value}"
        return
    fi
    for ((i = 0; i < ${#raw}; i++)); do
        char=${raw:i:1}
        if [[ ! ${char} =~ ^[0-9]$ ]]; then
            printf '%d\n' "${default_value}"
            return
        fi
        digit=$((10#${char}))
        value=$((value * 10 + digit))
        if (( value > max_value )); then
            printf '%d\n' "${max_value}"
            return
        fi
    done
    printf '%d\n' "${value}"
}

resolve_depth() {
    local reason=$1
    local global_depth=$2
    local capacity=$3
    local global_explicit=$4
    local submit_raw=$5
    local submit_default
    local depth=${global_depth}

    if [[ ${reason} == SUBMIT_3D && ${global_explicit} == 0 ]]; then
        if (( global_depth < 2 )); then
            submit_default=${global_depth}
        else
            submit_default=2
        fi
        depth="$(parse_cmdline_uint "${submit_raw}" "${submit_default}" \
            "${max_depth}")"
        if (( depth < 1 )); then
            depth=1
        fi
        if (( depth > capacity )); then
            depth=${capacity}
        fi
    fi
    printf '%d\n' "${depth}"
}

check_case() {
    local name=$1
    local reason=$2
    local global_depth=$3
    local capacity=$4
    local global_explicit=$5
    local submit_raw=$6
    local expected=$7
    local observed

    observed="$(resolve_depth "${reason}" "${global_depth}" "${capacity}" \
        "${global_explicit}" "${submit_raw}")"
    if [[ ${observed} != "${expected}" ]]; then
        printf 'virtio-gpu async depth case failed: %s expected=%s observed=%s\n' \
            "${name}" "${expected}" "${observed}" >&2
        exit 1
    fi
}

check_case global-explicit-bypass SUBMIT_3D 32 60 1 1 32
check_case non-submit-bypass FLUSH 32 60 0 1 32
check_case default-global1 SUBMIT_3D 1 60 0 "" 1
check_case default-global2 SUBMIT_3D 2 60 0 "" 2
check_case default-global32 SUBMIT_3D 32 60 0 "" 2
check_case explicit-submit1 SUBMIT_3D 32 60 0 1 1
check_case explicit-submit7 SUBMIT_3D 32 60 0 7 7
check_case parser-max-before-capacity SUBMIT_3D 32 128 0 65 60
check_case independent-capacity-clamp SUBMIT_3D 32 4 0 7 4

printf '%s\n' \
    'VIRTIO-GPU-ASYNC-SUBMIT-DEPTH-STATIC-PASS contiguous_resolver=PASS contiguous_parser_max=PASS global_explicit_bypass=PASS non_submit_bypass=PASS default_global1=1 default_global_ge2=2 explicit_submit1=1 explicit_submitN=PASS parser_max_before_capacity=PASS capacity_clamp=PASS source_mutation_old1_broaden_reorder_max_capacity=REJECT'
