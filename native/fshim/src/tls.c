/* Per-thread TLS relocation for the glibc-built game image.
 *
 * The game has 1448 bytes of local-exec TLS whose offsets are baked into the
 * instruction stream as `TP + 0x10000 + off` (tools/port.py rewrote the base
 * page).  bionic owns TPIDR_EL0 and expects its TCB at TP+0, so each thread
 * that is about to run game code gets a private block:
 *
 *     [ bionic TCB/TLS copy (span bytes) ][ zero pad ][ game TLS (base 0x10000) ]
 *     ^ -- new TPIDR_EL0
 *
 * Threads are intercepted by exporting our own pthread_create: libfshim.so is
 * the first DT_NEEDED of the game image, so the game's import binds to us.  We
 * call the real pthread_create via dlsym(RTLD_NEXT) and install TLS in the
 * child before its start routine runs.
 */
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <android/log.h>

#include "fshim.h"
#include "game_constants.h"

#define TAG "fshim"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static uintptr_t tp_read(void)
{
    uintptr_t value;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

static void tp_write(uintptr_t value)
{
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(value));
}

static int mapping_bounds(uintptr_t address, uintptr_t *start, uintptr_t *end)
{
    FILE *maps = fopen("/proc/self/maps", "re");
    if (!maps) {
        return 0;
    }
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long low = 0, high = 0;
        if (sscanf(line, "%lx-%lx", &low, &high) != 2) {
            continue;
        }
        if (address >= low && address < high) {
            *start = low;
            *end = high;
            found = 1;
            break;
        }
    }
    fclose(maps);
    return found;
}

void *fshim_install_tls(void)
{
    uintptr_t tp = tp_read();
    uintptr_t map_start = tp;
    uintptr_t map_end = tp + 0x2000;
    if (!mapping_bounds(tp, &map_start, &map_end)) {
        LOGW("cannot locate the mapping for TP %p; using a small window", (void *)tp);
        map_start = tp;
        map_end = tp + 0x2000;
    }
    /* The linker keeps a per-thread pointer at TP-8 and bionic may use other
       negative slots, so the copy starts a few pages below TP. */
    const size_t prefix = 0x4000;
    uintptr_t copy_start = (tp > map_start + prefix) ? tp - prefix : map_start;
    size_t span = (size_t)(map_end - copy_start);
    if (span > 0x10000) {
        span = 0x10000;
    }
    size_t total = span + FACTORIO_TLS_BASE + 0x4000;

    void *block = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (block == MAP_FAILED) {
        LOGE("mmap of %zu bytes for TLS failed: %s", total, strerror(errno));
        return NULL;
    }
    memcpy(block, (const void *)copy_start, span);

    uintptr_t heap_tp = (uintptr_t)block + (tp - copy_start);
    void *game_tls = (void *)(heap_tp + FACTORIO_TLS_BASE);
    tp_write(heap_tp);
    LOGI("tls installed: tp %p -> %p (copied 0x%zx from %p), game tls %p",
         (void *)tp, (void *)heap_tp, span, (void *)copy_start, game_tls);
    return game_tls;
}

/* ------------------------------------------------------- pthread interception */

typedef int (*pthread_create_fn)(pthread_t *, const pthread_attr_t *,
                                 void *(*)(void *), void *);

static pthread_create_fn real_pthread_create;

struct start_record {
    void *(*routine)(void *);
    void *argument;
};

static void *start_trampoline(void *opaque)
{
    struct start_record record = *(struct start_record *)opaque;
    free(opaque);
    if (!fshim_install_tls()) {
        LOGE("cannot install game TLS on new thread");
    }
    return record.routine(record.argument);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*routine)(void *), void *argument)
{
    if (!real_pthread_create) {
        real_pthread_create = (pthread_create_fn)dlsym(RTLD_NEXT, "pthread_create");
        if (!real_pthread_create) {
            LOGE("dlsym(RTLD_NEXT, pthread_create) failed: %s", dlerror());
            return EINVAL;
        }
    }
    struct start_record *record = malloc(sizeof(*record));
    if (!record) {
        return EAGAIN;
    }
    record->routine = routine;
    record->argument = argument;
    int result = real_pthread_create(thread, attr, start_trampoline, record);
    if (result != 0) {
        free(record);
    }
    return result;
}
