# Test Wrapper Architecture

This directory contains modular wrapper files for unit testing xv6 kernel components. Each wrapper provides mock implementations of kernel functions to enable isolated testing.

## Wrapper Files

### Page Wrappers (`page_wrappers.c`)
Consolidates all page-related mocking functionality:
- **Page allocation/deallocation**: `page_alloc`, `page_free`, `__page_alloc`, `__page_free`
- **Address conversion**: `__pa_to_page`, `__page_to_pa`  
- **Reference counting**: `page_ref_inc`, `page_ref_dec`, `page_refcnt`, `__page_ref_inc`, `__page_ref_dec`
- **Page locking**: `page_lock_acquire`, `page_lock_release`, `page_lock_assert_holding`, `page_lock_spin_unlock`
- **Page initialization**: `__page_init`
- **Mock page utilities**: `ut_make_mock_page`, `ut_destroy_mock_page`, `ut_destroy_mock_page_t`
- **Panic handling**: `__wrap_panic`

**Configuration**:
- When `UT_PAGE_TEST_BUILD` is defined: Uses managed memory range checking with `__managed_start`, `__managed_end`, and `__pages` array (for ut_page test)
- When undefined: Uses simple pointer casting (for other tests like ut_pcache)

**Previously**: Functionality was split between `page_wrappers.c` and `ut_page_wraps.c`. Now consolidated into a single file with conditional compilation.

### Spinlock Wrappers (`spinlock_wrappers.c`)
Mock spinlock operations:
- `spin_lock`, `spin_unlock`
- `spin_lock`, `spin_unlock`
- `spin_init`, `spin_holding`
- `push_off`, `pop_off`

### Process/Scheduler Wrappers (`proc_wrappers.c`)
Mock process management and scheduling:
- `cpuid`, `mycpu`, `myproc`
- `proc_lock`, `proc_unlock`, `proc_assert_holding`
- `sched_lock`, `sched_unlock`
- `scheduler_wakeup`, `scheduler_sleep`
- `kernel_proc_create`, `wakeup_proc`, `wakeup_on_chan`, `sleep_on_chan`

Uses cmocka APIs (`will_return`, `mock_type`, `mock_ptr_type`) for dynamic behavior control.

### Workqueue Wrappers (`workqueue_wrappers.c`)
Mock asynchronous work execution:
- `workqueue_create`, `queue_work`
- `init_work_struct`, `create_work_struct`, `free_work_struct`
- `pcache_test_run_pending_work` - public function to trigger work execution

**Key behavior**: Work is queued but not executed immediately. Call `wait_for_completion` to trigger synchronous execution, matching async behavior.

### Completion Wrappers (`completion_wrappers.c`)
Mock synchronization primitives:
- `completion_init`, `completion_reinit`
- `wait_for_completion`, `try_wait_for_completion`
- `complete`, `complete_all`
- `completion_done`

**Integration**: `wait_for_completion` calls `pcache_test_run_pending_work()` to execute pending work before checking completion status.

### Timer Wrappers (`timer_wrappers.c`)
Mock time-related functions:
- `get_jiffs` - returns incrementing tick count
- `sleep_ms` - no-op in tests

### KMM Wrappers (`kmm_wrappers.c`)
Mock kernel memory management:
- `kmm_alloc` - forwards to `test_malloc`
- `kmm_free` - forwards to `test_free`

### Slab Wrappers (`slab_wrappers.c`)
Mock slab allocator operations:
- `slab_cache_init`, `slab_cache_create`, `slab_cache_destroy`
- `slab_cache_shrink`
- `slab_alloc`, `slab_free`

### Panic Wrappers (`panic_wrappers.c`)
Mock panic and initialization functions:
- `__panic_start`, `__panic_end`
- `panic_state`, `panic_disable_bt`
- `printfinit`, `argint`

## Design Principles

### 1. Modularity
Each wrapper file focuses on a single subsystem. Files are independent and self-contained - no cross-wrapper function calls.

### 2. No Interdependencies
Wrappers manipulate data structures directly rather than calling other wrapper functions:
```c
// Good: Direct manipulation
page->lock.locked = 1;

// Bad: Cross-wrapper call
__wrap_spin_lock(&page->lock);  // Creates dependency on spinlock_wrappers.c
```

### 3. Cmocka Integration
Wrappers use cmocka APIs for dynamic behavior:
- `will_return()` - queue return value for mock
- `mock_type()` / `mock_ptr_type()` - retrieve queued value
- `expect_any()` - expect parameter (not commonly used - prefer `(void)param`)
- `check_expected()` - verify expected parameter

### 4. Conditional Compilation
Use preprocessor directives for test-specific behavior:
```c
#ifdef UT_PAGE_TEST_BUILD
    // Complex implementation for ut_page
#else
    // Simplified implementation for other tests
#endif
```

## Build Configuration

### Wrapper Selection (CMakeLists.txt)

Each test executable specifies which wrappers it needs:

```cmake
# Example: ut_page uses all wrappers
set(UT_PAGE_WRAPPER_SRC
    ${CMAKE_SOURCE_DIR}/src/wrappers/panic_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/spinlock_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/page_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/proc_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/kmm_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/slab_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/workqueue_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/completion_wrappers.c
    ${CMAKE_SOURCE_DIR}/src/wrappers/timer_wrappers.c
)

# Compile with special flag for page test
target_compile_definitions(ut_page PRIVATE UT_PAGE_TEST_BUILD=1)
```

### Function Wrapping (--wrap linker option)

Functions are intercepted using GNU ld's `--wrap` option:
```cmake
set(MOCK_TEST_WRAP_FUNCTIONS
    page_alloc
    page_free
    spin_lock
    # ... etc
)

# Generate --wrap options
foreach(EACH_WRAP ${MOCK_TEST_WRAP_FUNCTIONS})
    set(MOCK_WRAP_OPTIONS "${MOCK_WRAP_OPTIONS},--wrap=${EACH_WRAP}")
endforeach()
```

This causes calls to `function_name` to redirect to `__wrap_function_name`, and `__real_function_name` calls the original.

## Wrapper Patterns

### Basic Mock
```c
int __wrap_simple_function(void)
{
    return 42;  // Always return fixed value
}
```

### Cmocka Dynamic Mock
```c
int __wrap_dynamic_function(int param)
{
    (void)param;  // Suppress unused warning
    return mock_type(int);  // Return value set by will_return()
}

// In test:
will_return(__wrap_dynamic_function, 123);
int result = dynamic_function(0);  // Returns 123
```

### Passthrough Mock
```c
bool __wrap_my_alloc_passthrough = false;

void *__wrap_my_alloc(size_t size)
{
    if (__wrap_my_alloc_passthrough) {
        return __real_my_alloc(size);  // Call real implementation
    }
    return mock_ptr_type(void *);  // Or return mock value
}
```

### Conditional Mock
```c
void __wrap_platform_specific(int value)
{
#ifdef SPECIAL_TEST_MODE
    // Special behavior for certain tests
    global_test_value = value * 2;
#else
    // Normal mock behavior
    (void)value;
#endif
}
```

## Testing Workflow

1. **Setup**: Tests call `will_return()` to queue expected return values
2. **Execute**: Test code calls kernel functions
3. **Intercept**: Linker redirects to `__wrap_*` functions
4. **Mock**: Wrapper returns queued value via `mock_type()`
5. **Verify**: Test asserts results

## Changelog

### 2024-12-30: Page Wrapper Consolidation
- **Merged**: `ut_page_wraps.c` functionality into `page_wrappers.c`
- **Added**: Conditional compilation via `UT_PAGE_TEST_BUILD`
- **Removed**: Duplicate spinlock, CPU, and memory management functions from page_wrappers.c
- **Updated**: ut_page build to use modular wrapper architecture
- **Result**: Single unified page wrapper with test-specific behavior

### 2024-12-30: Workqueue/Completion Synchronization Fix
- **Changed**: Workqueue no longer executes work immediately in `queue_work()`
- **Changed**: `wait_for_completion()` now triggers work execution via `pcache_test_run_pending_work()`
- **Fixed**: Timing issue where `completion_reinit()` was resetting state after work completed
- **Result**: All 30 ut_pcache tests passing, including previously hanging flusher tests

## See Also

- [../README](../README) - Main test suite documentation
- [../include/ut_page_wraps.h](../include/ut_page_wraps.h) - Page wrapper function declarations
