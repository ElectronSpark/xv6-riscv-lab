#ifndef __KERNEL_CHROME_LIFECYCLE_H
#define __KERNEL_CHROME_LIFECYCLE_H

#include "cmdline.h"
#include "proc/thread.h"
#include "string.h"

static inline int chrome_trace_value_enabled(const char *key)
{
    char value[8];

    if (cmdline_get_param(key, value, sizeof(value)) != 0)
        return 0;
    return value[0] != '0' && value[0] != 'n' && value[0] != 'N';
}

static inline int chrome_lifecycle_trace_enabled(void)
{
    static int initialized;
    static int enabled;

    if (!initialized) {
        enabled = chrome_trace_value_enabled("chrome_lifecycle_trace") ||
                  chrome_trace_value_enabled("chrome_syscall_trace");
        initialized = 1;
    }
    return enabled;
}

static inline int chrome_thread_lifecycle_trace_enabled(void)
{
    return chrome_trace_value_enabled("chrome_thread_lifecycle_trace");
}

static inline int chrome_exec_syscall_trace_enabled(void)
{
    return chrome_trace_value_enabled("chrome_exec_syscall_trace");
}

static inline int chrome_lifecycle_string_match(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return 0;
    return strncmp(value, "chrome", 6) == 0 ||
           strstr(value, "chromium") != NULL ||
           strstr(value, "wayland-chromium") != NULL ||
           strstr(value, "webkit") != NULL ||
           strstr(value, "WebKit") != NULL;
}

static inline int chrome_lifecycle_thread_match(struct thread *p)
{
    if (p == NULL)
        return 0;
    if (chrome_lifecycle_string_match(p->name))
        return 1;
    if (p->thread_group != NULL &&
        chrome_lifecycle_string_match(p->thread_group->exec_path))
        return 1;
    return 0;
}

static inline int chrome_lifecycle_network_service_match(struct thread *p)
{
    if (p == NULL || p->thread_group == NULL)
        return 0;
    uint32 roles = __atomic_load_n(&p->thread_group->chrome_trace_roles,
                                   __ATOMIC_RELAXED);
    return (roles & TG_CHROME_TRACE_NETWORK_SERVICE) != 0;
}

static inline int chrome_lifecycle_child_process_match(struct thread *p)
{
    if (p == NULL || p->thread_group == NULL)
        return 0;
    uint32 roles = __atomic_load_n(&p->thread_group->chrome_trace_roles,
                                   __ATOMIC_RELAXED);
    return (roles & TG_CHROME_TRACE_CHILD_PROCESS) != 0;
}

#endif /* __KERNEL_CHROME_LIFECYCLE_H */
