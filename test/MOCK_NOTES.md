# Mock Implementation Notes

When building the unit tests for the XV6 kernel functions, we need to ensure our mocked functions are properly linked instead of the real kernel functions. This is achieved through various mechanisms:

1. **Function Wrapping**: Our mock implementations use the naming convention `__wrap_function_name` to replace the real `function_name` in the kernel.

2. **Linker Flags**: We need to use the `-Wl,--wrap=function_name` linker flag for each function we want to mock.

3. **External Variables**: For kernel global variables, we provide mock implementations in our test files.

## List of Functions to Mock

The following functions should be properly mocked using the wrap mechanism:

- `page_ref_inc`
- `page_ref_dec`
- `__page_ref_inc`
- `__page_ref_dec`
- `page_ref_count`
- `__pa_to_page`
- `__page_to_pa`
- `page_refcnt`
- `page_lock_acquire`
- `page_lock_release`
- `__page_alloc`
- `__page_free`
- `page_alloc`
- `page_free`
- `page_buddy_init`

## Variables to Mock

The following global variables need mock implementations:

- `__buddy_pools`
- `__managed_start`
- `__managed_end`
- `__pages`

## CMake Integration

Update the CMakeLists.txt to include the necessary wrap flags for the linker.

```cmake
# Example linker flags
set(WRAP_FLAGS 
    "-Wl,--wrap=page_ref_inc"
    "-Wl,--wrap=page_ref_dec"
    # ... other function wrappers
)

target_link_options(ut_mock PRIVATE ${WRAP_FLAGS})
```
