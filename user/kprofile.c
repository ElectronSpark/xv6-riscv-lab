#include "kernel/inc/types.h"
#include "kernel/inc/kstats.h"
#include "user/user.h"

static void print_delta(const char *name, uint64 after, uint64 before)
{
    printf("%-28s %lu\n", name, (unsigned long)(after - before));
}

static void print_tick_delta_ms(const char *name, uint64 after, uint64 before,
                                uint64 timebase_freq)
{
    uint64 delta = after - before;
    uint64 ms = timebase_freq ? (delta * 1000ULL) / timebase_freq : 0;
    printf("%-28s %lu\n", name, (unsigned long)ms);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: kprofile <command> [args...]\n");
        exit(1);
    }

    struct kstats before;
    struct kstats after;

    if (kstats(&before) < 0) {
        printf("kprofile: kstats(before) failed\n");
        exit(1);
    }

    int pid = fork();
    if (pid < 0) {
        printf("kprofile: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        exec(argv[1], &argv[1]);
        printf("kprofile: exec %s failed\n", argv[1]);
        exit(1);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (kstats(&after) < 0) {
        printf("kprofile: kstats(after) failed\n");
        exit(1);
    }

    printf("elapsed_ms                  %lu\n",
           (unsigned long)(after.uptime_ms - before.uptime_ms));
    print_delta("vfs_lookup_calls", after.vfs_lookup_calls,
                before.vfs_lookup_calls);
    print_delta("vfs_lookup_dcache_hits", after.vfs_lookup_dcache_hits,
                before.vfs_lookup_dcache_hits);
    print_delta("vfs_lookup_negative_hits", after.vfs_lookup_negative_hits,
                before.vfs_lookup_negative_hits);
    print_delta("vfs_lookup_cache_misses", after.vfs_lookup_cache_misses,
                before.vfs_lookup_cache_misses);
    print_delta("vfs_lookup_driver_calls", after.vfs_lookup_driver_calls,
                before.vfs_lookup_driver_calls);
    print_tick_delta_ms("vfs_lookup_driver_ms",
                        after.vfs_lookup_driver_ticks,
                        before.vfs_lookup_driver_ticks,
                        after.timebase_freq);
    print_delta("vm_copyin_calls", after.vm_copyin_calls,
                before.vm_copyin_calls);
    print_delta("vm_copyout_calls", after.vm_copyout_calls,
                before.vm_copyout_calls);
    print_delta("vm_copyin_bytes", after.vm_copyin_bytes,
                before.vm_copyin_bytes);
    print_delta("vm_copyout_bytes", after.vm_copyout_bytes,
                before.vm_copyout_bytes);
    print_delta("vm_vma_validate_calls", after.vm_vma_validate_calls,
                before.vm_vma_validate_calls);
    print_tick_delta_ms("vm_vma_validate_ms",
                        after.vm_vma_validate_ticks,
                        before.vm_vma_validate_ticks,
                        after.timebase_freq);
    print_delta("vm_file_faults", after.vm_file_faults,
                before.vm_file_faults);
    print_tick_delta_ms("vm_copyin_ms", after.vm_copyin_ticks,
                        before.vm_copyin_ticks, after.timebase_freq);
    print_tick_delta_ms("vm_copyout_ms", after.vm_copyout_ticks,
                        before.vm_copyout_ticks, after.timebase_freq);
    print_delta("ext4_pcache_read_page_calls",
                after.ext4_pcache_read_page_calls,
                before.ext4_pcache_read_page_calls);
    print_delta("ext4_pcache_pages_filled",
                after.ext4_pcache_pages_filled,
                before.ext4_pcache_pages_filled);
    print_delta("ext4_pcache_readahead_pages",
                after.ext4_pcache_readahead_pages,
                before.ext4_pcache_readahead_pages);
    print_tick_delta_ms("ext4_pcache_read_page_ms",
                        after.ext4_pcache_read_page_ticks,
                        before.ext4_pcache_read_page_ticks,
                        after.timebase_freq);
    print_delta("ext4_fault_calls", after.ext4_fault_calls,
                before.ext4_fault_calls);
    print_delta("ext4_fault_zero_copy", after.ext4_fault_zero_copy,
                before.ext4_fault_zero_copy);
    print_delta("ext4_fault_partial_copy", after.ext4_fault_partial_copy,
                before.ext4_fault_partial_copy);
    print_tick_delta_ms("ext4_fault_ms", after.ext4_fault_ticks,
                        before.ext4_fault_ticks, after.timebase_freq);
    print_delta("sys_open_calls", after.sys_open_calls,
                before.sys_open_calls);
    print_tick_delta_ms("sys_open_ms", after.sys_open_ticks,
                        before.sys_open_ticks, after.timebase_freq);
    print_delta("sys_fstat_calls", after.sys_fstat_calls,
                before.sys_fstat_calls);
    print_tick_delta_ms("sys_fstat_ms", after.sys_fstat_ticks,
                        before.sys_fstat_ticks, after.timebase_freq);
    print_delta("sys_lseek_calls", after.sys_lseek_calls,
                before.sys_lseek_calls);
    print_tick_delta_ms("sys_lseek_ms", after.sys_lseek_ticks,
                        before.sys_lseek_ticks, after.timebase_freq);
    print_delta("sys_pread64_calls", after.sys_pread64_calls,
                before.sys_pread64_calls);
    print_tick_delta_ms("sys_pread64_ms", after.sys_pread64_ticks,
                        before.sys_pread64_ticks, after.timebase_freq);
    print_delta("sys_openat_calls", after.sys_openat_calls,
                before.sys_openat_calls);
    print_tick_delta_ms("sys_openat_ms", after.sys_openat_ticks,
                        before.sys_openat_ticks, after.timebase_freq);
    print_delta("sys_fstatat_calls", after.sys_fstatat_calls,
                before.sys_fstatat_calls);
    print_tick_delta_ms("sys_fstatat_ms", after.sys_fstatat_ticks,
                        before.sys_fstatat_ticks, after.timebase_freq);
    print_delta("sys_faccessat_calls", after.sys_faccessat_calls,
                before.sys_faccessat_calls);
    print_tick_delta_ms("sys_faccessat_ms", after.sys_faccessat_ticks,
                        before.sys_faccessat_ticks, after.timebase_freq);
    print_delta("sys_read_calls", after.sys_read_calls,
                before.sys_read_calls);
    print_tick_delta_ms("sys_read_ms", after.sys_read_ticks,
                        before.sys_read_ticks, after.timebase_freq);
    print_delta("sys_readv_calls", after.sys_readv_calls,
                before.sys_readv_calls);
    print_tick_delta_ms("sys_readv_ms", after.sys_readv_ticks,
                        before.sys_readv_ticks, after.timebase_freq);
    print_delta("sys_getdents_calls", after.sys_getdents_calls,
                before.sys_getdents_calls);
    print_tick_delta_ms("sys_getdents_ms", after.sys_getdents_ticks,
                        before.sys_getdents_ticks, after.timebase_freq);
    print_delta("sys_readlinkat_calls", after.sys_readlinkat_calls,
                before.sys_readlinkat_calls);
    print_tick_delta_ms("sys_readlinkat_ms", after.sys_readlinkat_ticks,
                        before.sys_readlinkat_ticks, after.timebase_freq);
    print_delta("sys_mmap_calls", after.sys_mmap_calls,
                before.sys_mmap_calls);
    print_tick_delta_ms("sys_mmap_ms", after.sys_mmap_ticks,
                        before.sys_mmap_ticks, after.timebase_freq);
    print_delta("sys_munmap_calls", after.sys_munmap_calls,
                before.sys_munmap_calls);
    print_tick_delta_ms("sys_munmap_ms", after.sys_munmap_ticks,
                        before.sys_munmap_ticks, after.timebase_freq);
    print_delta("sys_mprotect_calls", after.sys_mprotect_calls,
                before.sys_mprotect_calls);
    print_tick_delta_ms("sys_mprotect_ms", after.sys_mprotect_ticks,
                        before.sys_mprotect_ticks, after.timebase_freq);
    print_delta("sys_brk_calls", after.sys_brk_calls,
                before.sys_brk_calls);
    print_tick_delta_ms("sys_brk_ms", after.sys_brk_ticks,
                        before.sys_brk_ticks, after.timebase_freq);
    print_delta("sys_clock_gettime_calls", after.sys_clock_gettime_calls,
                before.sys_clock_gettime_calls);
    print_tick_delta_ms("sys_clock_gettime_ms",
                        after.sys_clock_gettime_ticks,
                        before.sys_clock_gettime_ticks,
                        after.timebase_freq);
    print_delta("sys_gettimeofday_calls", after.sys_gettimeofday_calls,
                before.sys_gettimeofday_calls);
    print_tick_delta_ms("sys_gettimeofday_ms",
                        after.sys_gettimeofday_ticks,
                        before.sys_gettimeofday_ticks,
                        after.timebase_freq);
    print_delta("sys_getrandom_calls", after.sys_getrandom_calls,
                before.sys_getrandom_calls);
    print_tick_delta_ms("sys_getrandom_ms", after.sys_getrandom_ticks,
                        before.sys_getrandom_ticks, after.timebase_freq);
    print_delta("exec_calls", after.exec_calls,
                before.exec_calls);
    print_tick_delta_ms("exec_ms", after.exec_ticks,
                        before.exec_ticks, after.timebase_freq);

    exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}