# Test Wrapper Module Organization

This document describes the modular organization of test wrappers for the xv6-riscv kernel unit tests.

## Design Principles

1. **Modular Organization**: Wrappers are grouped by kernel subsystem
2. **Independent Implementations**: Each wrapper module is self-contained with no dependencies on other wrappers
3. **Selective Inclusion**: Tests only link the wrapper modules they need
4. **Mock Behavior**: Wrappers provide minimal mock implementations suitable for unit testing

### Why No Interdependencies?

For mock tests, wrapper interdependencies are avoided because:
- Each test should only link the wrappers it needs
- Cross-wrapper calls create unnecessary coupling
- Mock implementations should be simple and self-contained
- Direct structure manipulation is sufficient for testing

Instead of calling other wrapper functions, each wrapper directly manipulates
the structures it needs. For example, instead of calling `__wrap_spin_acquire()`,
a wrapper will directly set `lock->locked = 1`.

## Wrapper Modules

All wrapper implementations are located in `/test/src/wrappers/` and are organized by functionality:

## Wrapper Tracking Infrastructure

Some wrappers support optional tracking for test instrumentation. The tracking infrastructure is defined in `/test/include/wrapper_tracking.h` and provides:

### Tracking Types

**spinlock_tracking_t**: Tracks spinlock operations
- `spin_init_count`, `spin_acquire_count`, `spin_release_count`
- `last_spin_init`, `last_spin_acquire`, `last_spin_release`
- `last_spin_name`

**proc_queue_tracking_t**: Tracks proc_queue operations
- `queue_init_count`, `queue_wait_count`, `queue_wakeup_count`, `queue_wakeup_all_count`
- `last_queue_init`, `last_queue_wait`, `last_queue_wakeup`, `last_queue_wakeup_all`
- `wait_return`, `wakeup_return`, `wakeup_all_return` - Control return values
- `wait_callback` - Custom callback for test-specific behavior
- `next_wakeup_proc` - Control which process is woken

### Usage in Tests

Tests that need instrumentation:
1. Declare tracking structure instances
2. Call `wrapper_tracking_enable_*()` in test setup
3. Access tracking fields for assertions
4. Call `wrapper_tracking_disable_*()` if needed

Tracking is completely optional - wrappers work without it. When tracking is disabled (tracking pointer is NULL), wrappers provide basic mock behavior only.

## Wrapper Module Reference

### 1. panic_wrappers.c
**Purpose:** Panic and error handling
**Functions:**
- `__wrap_panic_disable_bt()` - Disable backtrace
- `__wrap___panic_start()` - Panic initialization
- `__wrap___panic_end()` - Panic termination
- `__wrap_panic_state()` - Get panic state
- `__wrap_printfinit()` - Print initialization stub

**Used by:** All tests that include kernel code

### 2. spinlock_wrappers.c
**Purpose:** Spinlock synchronization primitives with optional tracking
**Functions:**
- `__wrap_spin_init()` - Initialize spinlock
- `__wrap_spin_acquire()` - Acquire spinlock
- `__wrap_spin_release()` - Release spinlock
- `__wrap_spin_holding()` - Check if holding spinlock
- `__wrap_spin_lock()` - Lock (alias for acquire)
- `__wrap_spin_unlock()` - Unlock (alias for release)

**Tracking Support:**
- `wrapper_tracking_enable_spinlock()` - Enable operation tracking
- `wrapper_tracking_disable_spinlock()` - Disable operation tracking
- Tracks: init/acquire/release counts, last operations

**Used by:** ut_page, ut_pcache, ut_semaphore, ut_rwlock, tests involving locking

### 3. page_wrappers.c
**Purpose:** Page allocation and management
**Functions:**
- `__wrap_page_lock_acquire/release()` - Page-level locking
- `__wrap_page_ref_inc/dec()` - Reference counting
- `__wrap_page_alloc/free()` - Page allocation
- `__wrap___page_alloc/__page_free()` - Internal page allocation
- `__wrap___page_to_pa/__pa_to_page()` - Address conversion

**Test utilities:**
- `pcache_test_fail_next_page_alloc()` - Simulate allocation failure

**Used by:** ut_page, ut_pcache

### 4. slab_wrappers.c
**Purpose:** Slab allocator for small objects
**Functions:**
- `__wrap_slab_cache_init/create/destroy()` - Cache management
- `__wrap_slab_cache_shrink()` - Cache shrinking
- `__wrap_slab_alloc/free()` - Object allocation

**Test utilities:**
- `pcache_test_fail_next_slab_alloc()` - Simulate allocation failure

**Used by:** ut_page, ut_pcache

### 5. workqueue_wrappers.c
**Purpose:** Asynchronous work queue management
**Functions:**
- `__wrap_workqueue_create()` - Create work queue
- `__wrap_queue_work()` - Queue work item
- `__wrap_init_work_struct()` - Initialize work structure
- `__wrap_create/free_work_struct()` - Work structure lifecycle

**Test utilities:**
- `pcache_test_fail_next_queue_work()` - Simulate queue failure

**Used by:** ut_pcache

### 6. completion_wrappers.c
**Purpose:** Completion synchronization primitives
**Functions:**
- `__wrap_completion_init/reinit()` - Initialization
- `__wrap_wait_for_completion()` - Wait for completion
- `__wrap_try_wait_for_completion()` - Non-blocking wait
- `__wrap_complete/complete_all()` - Signal completion
- `__wrap_completion_done()` - Check completion status

**Used by:** ut_pcache

### 7. timer_wrappers.c
**Purpose:** Timer and sleep functionality
**Functions:**
- `__wrap_get_jiffs()` - Get jiffies (mock timer ticks)
- `__wrap_sleep_ms()` - Sleep for milliseconds (no-op in tests)

**Used by:** ut_pcache

### 8. proc_wrappers.c
**Purpose:** Process management, scheduling, and proc_queue operations with optional tracking
**Functions:**
- `__wrap_mycpu/myproc()` - Get current CPU/process
- `__wrap_proc_lock/unlock()` - Process locking
- `__wrap_proc_assert_holding()` - Assert lock held
- `__wrap_kernel_proc_create()` - Create kernel process
- `__wrap_wakeup_proc/wakeup_on_chan()` - Wake up processes
- `__wrap_sleep_on_chan()` - Sleep on channel
- `__wrap_proc_queue_init()` - Initialize proc_queue
- `__wrap_proc_queue_wait()` - Wait on proc_queue
- `__wrap_proc_queue_wakeup()` - Wake one process from queue
- `__wrap_proc_queue_wakeup_all()` - Wake all processes from queue

**Tracking Support:**
- `wrapper_tracking_enable_proc_queue()` - Enable operation tracking
- `wrapper_tracking_disable_proc_queue()` - Disable operation tracking
- Tracks: init/wait/wakeup/wakeup_all counts, return values, custom callbacks
- Supports custom wait_callback for test-specific behavior

**Test utilities:**
- `pcache_test_set_break_on_sleep()` - Control sleep behavior
- `pcache_test_set_max_sleep_calls()` - Limit sleep calls

**Used by:** ut_semaphore, ut_rwlock, ut_pcache

### 9. cpu_wrappers.c
**Purpose:** CPU and interrupt management
**Functions:**
- `__wrap_cpuid()` - Get CPU ID (always returns 0)
- `__wrap_push_off/pop_off()` - Interrupt disable/enable

**Used by:** ut_page, ut_pcache

### 10. kmm_wrappers.c
**Purpose:** Kernel memory management
**Functions:**
- `__wrap_kmm_alloc()` - Kernel memory allocation (uses malloc)
- `__wrap_kmm_free()` - Kernel memory free (uses free)

**Used by:** ut_page

### 11. syscall_wrappers.c
**Purpose:** System call stubs
**Functions:**
- `__wrap_argint()` - Get syscall integer argument (stub)

**Used by:** Tests that include syscall-related code

## Usage in Tests

### Example: ut_page
Uses the following wrapper modules:
- panic_wrappers.c
- spinlock_wrappers.c (via ut_page_wraps.c)
- page_wrappers.c (via ut_page_wraps.c)
- cpu_wrappers.c (via ut_page_wraps.c)
- slab_wrappers.c
- workqueue_wrappers.c
- completion_wrappers.c
- timer_wrappers.c
- kmm_wrappers.c

### Example: ut_semaphore
Uses the following wrapper modules:
- panic_wrappers.c
- spinlock_wrappers.c (with tracking enabled)
- proc_wrappers.c (with tracking enabled)

Tracking enabled for test instrumentation and assertion verification.

### Example: ut_rwlock
Uses the following wrapper modules:
- panic_wrappers.c
- spinlock_wrappers.c (with tracking enabled)
- proc_wrappers.c (with tracking enabled)

Tracking enabled for test instrumentation and assertion verification.

### Example: ut_pcache
Uses the following wrapper modules:
- panic_wrappers.c
- spinlock_wrappers.c
- page_wrappers.c
- slab_wrappers.c
- workqueue_wrappers.c
- completion_wrappers.c
- timer_wrappers.c
- proc_wrappers.c

## CMakeLists.txt Integration

Wrappers are defined in CMakeLists.txt as:

```cmake
set(WRAPPER_SRC
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/page_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/slab_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/workqueue_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/completion_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/timer_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/kmm_wrappers.c
)
```

Tests include only the wrapper modules they need. For example:

```cmake
add_executable(ut_semaphore
    ${CMAKE_SOURCE_DIR}/src/ut_semaphore_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/lock/semaphore.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
)

add_executable(ut_rwlock
    ${CMAKE_SOURCE_DIR}/src/ut_rwlock_main.c
    ${CMAKE_SOURCE_DIR}/../kernel/lock/rwlock.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
)
```

## Linker Wrap Options

All wrapper functions use the `--wrap` linker option. For each wrapped function, the linker:
- Replaces calls to `function` with calls to `__wrap_function`
- Makes the original function available as `__real_function`

Example link options:
```cmake
target_link_options(ut_semaphore PRIVATE 
    -Wl,--wrap=mycpu,--wrap=myproc
    -Wl,--wrap=spin_init,--wrap=spin_acquire,--wrap=spin_release
    -Wl,--wrap=proc_queue_init,--wrap=proc_queue_wait
    -Wl,--wrap=proc_queue_wakeup,--wrap=proc_queue_wakeup_all
)
```

## Adding New Wrappers

To add a new wrapper module:

1. Create the wrapper file in `/test/src/wrappers/`
2. Implement functions with `__wrap_` prefix
3. Add the file to appropriate variable in CMakeLists.txt
4. Add `--wrap=function_name` to linker options
5. Document in this file

## Test Utilities

Some wrappers provide test utilities for controlling behavior:
- `pcache_test_fail_next_page_alloc()` - Simulate page allocation failure
- `pcache_test_fail_next_slab_alloc()` - Simulate slab allocation failure
- `pcache_test_fail_next_queue_work()` - Simulate work queue failure
- `pcache_test_set_break_on_sleep()` - Control sleep behavior
- `pcache_test_set_max_sleep_calls()` - Limit sleep iterations

These allow tests to exercise error paths and edge cases.
