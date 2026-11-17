/* TAL audio_subsys C++ runtime shim for the WL82 vendor SDK.
 *
 * The TAL archive is LLVM bitcode built with the vendor clang 4.0.1 for
 * -target pi32v2, but it was built against a fuller libc++/libc++abi
 * configuration than the one the WL82 demo links. Pulling it in exposes a
 * handful of symbols the vendor image never needed:
 *
 *   - operator new/delete (the archive allocates with C++ `new`)
 *   - pthread_* used by libc++abi's __cxa_guard static-init guard
 *   - abort(), _malloc_r()/... newlib reentrant wrappers pulled by libc++'s
 *     locale/string objects
 *   - fprintf()/snprintf() error-formatting helpers
 *   - __cxa_pure_virtual(), which cxx_runtime.cpp defines but which the
 *     final LTO link does not retain
 *
 * The KWS engine runs from one Tuya task and never throws; single-threaded
 * no-op mutex/cond stubs are therefore sufficient. This file is compiled as
 * C and uses asm labels so the C++ mangled names match without pulling in a
 * second C++ translation unit.
 */
#include <stdarg.h>
#include <stddef.h>

typedef unsigned int u32;

extern void *malloc(u32 size);
extern void free(void *ptr);
extern void *realloc(void *ptr, u32 size);

/* Note: abort, __cxa_pure_virtual, _malloc_r/_free_r/_calloc_r, fprintf,
 * snprintf and the pthread_mutex_* family are already defined by the vendor
 * image (cxx_runtime.cpp, os_api.c.o, printf-stdarg.c.o and
 * FreeRTOS_POSIX_pthread_mutex.c.o). The TAL group rescans those archives, so
 * this file only supplies the symbols the vendor image genuinely lacks. */

/* newlib reentrant realloc entry point. */
void *_realloc_r(void *reent, void *ptr, u32 size)
{
    (void)reent;
    return realloc(ptr, size);
}

/* The vendor FreeRTOS POSIX layer already provides pthread_mutex_*; only the
 * condition-variable and TLS entry points used by libc++abi's guard are
 * missing. The engine is entered from one Tuya task, so these are no-ops. */
int pthread_cond_broadcast(void *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_wait(void *cond, void *mutex)
{
    (void)cond;
    (void)mutex;
    return 0;
}

int pthread_cond_init(void *cond, const void *attr)
{
    (void)cond;
    (void)attr;
    return 0;
}

int pthread_cond_destroy(void *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_signal(void *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_timedwait(void *cond, void *mutex, const void *abstime)
{
    (void)cond;
    (void)mutex;
    (void)abstime;
    return 0;
}

int pthread_key_create(unsigned int *key, void (*destructor)(void *))
{
    (void)destructor;
    if (key != 0) *key = 0;
    return 0;
}

int pthread_key_delete(unsigned int key)
{
    (void)key;
    return 0;
}

void *pthread_getspecific(unsigned int key)
{
    (void)key;
    return 0;
}

int pthread_setspecific(unsigned int key, const void *value)
{
    (void)key;
    (void)value;
    return 0;
}

int pthread_detach(void *thread)
{
    (void)thread;
    return 0;
}

int pthread_once(int *once, void (*init)(void))
{
    if (once != 0 && *once == 0) {
        *once = 1;
        if (init != 0) init();
    }
    return 0;
}

unsigned int strftime(char *buf, u32 size, const char *fmt, const void *tm)
{
    (void)buf;
    (void)size;
    (void)fmt;
    (void)tm;
    return 0;
}

/* C++ operator new/delete, declared with the pi32v2 mangled names. */
void *tal_operator_new(u32 size) __asm__("_Znwj");
void *tal_operator_new(u32 size)
{
    return malloc(size);
}

void *tal_operator_new_array(u32 size) __asm__("_Znaj");
void *tal_operator_new_array(u32 size)
{
    return malloc(size);
}

void tal_operator_delete(void *ptr) __asm__("_ZdlPv");
void tal_operator_delete(void *ptr)
{
    free(ptr);
}

void tal_operator_delete_array(void *ptr) __asm__("_ZdaPv");
void tal_operator_delete_array(void *ptr)
{
    free(ptr);
}

void tal_operator_delete_sized(void *ptr, u32 size) __asm__("_ZdlPvj");
void tal_operator_delete_sized(void *ptr, u32 size)
{
    (void)size;
    free(ptr);
}

void tal_operator_delete_array_sized(void *ptr, u32 size) __asm__("_ZdaPvj");
void tal_operator_delete_array_sized(void *ptr, u32 size)
{
    (void)size;
    free(ptr);
}
