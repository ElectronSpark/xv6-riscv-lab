set(TIMER_C_SRCS
    timer.c
    sched_timer.c
    goldfish_rtc.c
)

add_library(timer OBJECT
    ${TIMER_C_SRCS}
)

# Add build directory to include path for generated headers
target_include_directories(timer PRIVATE ${CMAKE_BINARY_DIR}/kernel/inc)
