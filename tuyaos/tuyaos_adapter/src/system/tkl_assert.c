#include <stdio.h>

#include "asm/cpu.h"

void __assert_func(const char *file, int line, const char *function, const char *expression)
{
    printf("assertion failed: %s:%d (%s): %s\n", file ? file : "?", line,
           function ? function : "?", expression ? expression : "?");
    system_reset();
    for (;;) {
    }
}

int system(const char *command)
{
    (void)command;
    return -1;
}

/* No random32() here: the vendor SDK already provides it in
 * cpu/<chip>/liba/common_lib.a (rand.c.o, also in libcurl.a and libsip.a).
 * Defining it in the adapter collides with that archive member as soon as a
 * link pulls it — switch_demo does not, output_speaker does, which is why the
 * collision can go unnoticed. The vendor's definition is the one to use. */
