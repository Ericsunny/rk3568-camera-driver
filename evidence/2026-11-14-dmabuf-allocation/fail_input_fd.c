/* Test-only interposition: fail the first main-loop shared buffer fd lookup. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <rockchip/mpp_buffer.h>

int mpp_buffer_get_fd_with_caller(MppBuffer buffer, const char *caller)
{
    typedef int (*getter)(MppBuffer, const char *);
    static getter real_get;
    static int hit;
    if (!real_get)
        *(void **)(&real_get) = dlsym(RTLD_NEXT, "mpp_buffer_get_fd_with_caller");
    if (!real_get)
        return -1;
    if (!hit && caller && !strcmp(caller, "main")) {
        hit = 1;
        fprintf(stderr, "[fault] shared main-loop fd lookup returned -1\n");
        return -1;
    }
    return real_get(buffer, caller);
}
