// Wraps zstd's reference decoder (doc/educational_decoder, v1.5.7, BSD) for
// untrusted input. The reference decoder calls exit() on corrupt data and
// never frees on that path; here every allocation is tracked per thread and
// released, and exit() unwinds back to the caller instead.
#include "bounded_zstd.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#define SPOOL_ZSTD_TLS __declspec(thread)
#else
#define SPOOL_ZSTD_TLS _Thread_local
#endif

typedef struct allocation {
    struct allocation *previous;
    struct allocation *next;
} allocation;

static SPOOL_ZSTD_TLS jmp_buf *unwind;
static SPOOL_ZSTD_TLS allocation *allocations;

static void *tracked(allocation *block)
{
    if (!block)
        return NULL;
    block->previous = NULL;
    block->next = allocations;
    if (allocations)
        allocations->previous = block;
    allocations = block;
    return block + 1;
}

static void *bounded_malloc(size_t size)
{
    return size > SIZE_MAX - sizeof(allocation) ? NULL : tracked(malloc(sizeof(allocation) + size));
}

static void *bounded_calloc(size_t count, size_t size)
{
    if (size && count > (SIZE_MAX - sizeof(allocation)) / size)
        return NULL;
    void *pointer = tracked(malloc(sizeof(allocation) + count * size));
    if (pointer)
        memset(pointer, 0, count * size);
    return pointer;
}

static void bounded_free(void *pointer)
{
    if (!pointer)
        return;
    allocation *block = (allocation *)pointer - 1;
    if (block->previous)
        block->previous->next = block->next;
    else
        allocations = block->next;
    if (block->next)
        block->next->previous = block->previous;
    free(block);
}

static void release_all(void)
{
    while (allocations) {
        allocation *next = allocations->next;
        free(allocations);
        allocations = next;
    }
}

#define ZDEC_NO_MESSAGE
#define malloc bounded_malloc
#define calloc bounded_calloc
#define free bounded_free
#define exit(code) longjmp(*unwind, 1)
#include "zstd_decompress.c"
#undef malloc
#undef calloc
#undef free
#undef exit

size_t spool_zstd_content_size(const void *source, size_t sourceLength)
{
    jmp_buf here;
    jmp_buf *outer = unwind;
    size_t size = (size_t)-1;
    unwind = &here;
    if (setjmp(here) == 0)
        size = ZSTD_get_decompressed_size(source, sourceLength);
    unwind = outer;
    release_all();
    return size;
}

int spool_zstd_decompress(const void *source, size_t sourceLength, void *destination, size_t capacity, size_t *written)
{
    jmp_buf here;
    jmp_buf *outer = unwind;
    int result = 0;
    unwind = &here;
    if (setjmp(here) == 0)
        *written = ZSTD_decompress(destination, capacity, source, sourceLength);
    else
        result = -1;
    unwind = outer;
    release_all();
    return result;
}
