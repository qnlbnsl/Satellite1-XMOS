// What the x86 build of led_pointer.c adds for a ctypes caller.
#include <stddef.h>

#include "led_pointer.h"

size_t led_pointer_size(void)
{
    return sizeof(led_pointer_t);
}

size_t led_glide_size(void)
{
    return sizeof(led_glide_t);
}
