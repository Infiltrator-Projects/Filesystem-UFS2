/* SPDX-License-Identifier: GPL-3.0-or-later
 * UFS2 freestanding allocation and byte primitives. The authenticated
 * Common V2 read-only host supplies allocations: no glibc or EFI imports.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef union {
    max_align_t alignment;
    struct {
        const IfsFsHostRuntimeV2 *host;
        size_t length;
    } owner;
} Ufs2Allocation;
static const IfsFsHostRuntimeV2 *active_host;
void ifs_ufs_firmware_bind_host(const IfsFsHostRuntimeV2 *host)
{
    active_host = host;
}
void *memcpy(void *restrict dst, const void *restrict src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < count; ++i) d[i] = s[i];
    return dst;
}
void *memmove(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d > (uintptr_t)s &&
        (uintptr_t)d - (uintptr_t)s < count) {
        for (size_t i = count; i != 0; --i) d[i - 1] = s[i - 1];
    } else {
        for (size_t i = 0; i < count; ++i) d[i] = s[i];
    }
    return dst;
}
void *memset(void *dst, int byte, size_t count)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < count; ++i) d[i] = (unsigned char)byte;
    return dst;
}
int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < count; ++i)
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}
size_t strlen(const char *str)
{
    size_t length = 0;
    while (str[length]) ++length;
    return length;
}
int strcmp(const char *a, const char *b)
{
    for (; *a && *a == *b; ++a, ++b) {}
    return (unsigned char)*a - (unsigned char)*b;
}
void *malloc(size_t count)
{
    if (!active_host || !active_host->allocate || !active_host->release ||
        count > SIZE_MAX - sizeof(Ufs2Allocation))
        return NULL;
    if (!count) count = 1;
    Ufs2Allocation *block = active_host->allocate(active_host->context,
                                                  sizeof(*block) + count);
    if (!block) return NULL;
    block->owner.host = active_host;
    block->owner.length = count;
    return block + 1;
}
void free(void *ptr)
{
    if (!ptr) return;
    Ufs2Allocation *block = (Ufs2Allocation *)ptr - 1;
    const IfsFsHostRuntimeV2 *host = block->owner.host;
    host->release(host->context, block);
}
void *calloc(size_t count, size_t width)
{
    if (width && count > SIZE_MAX / width) return NULL;
    size_t bytes = count * width;
    void *ptr = malloc(bytes);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}
void *realloc(void *ptr, size_t count)
{
    if (!ptr) return malloc(count);
    if (!count) { free(ptr); return NULL; }
    Ufs2Allocation *old = (Ufs2Allocation *)ptr - 1;
    const IfsFsHostRuntimeV2 *host = old->owner.host;
    if (count > SIZE_MAX - sizeof(*old)) return NULL;
    Ufs2Allocation *replacement = host->allocate(host->context,
                                                  sizeof(*old) + count);
    if (!replacement) return NULL;
    replacement->owner.host = host;
    replacement->owner.length = count;
    const size_t take = old->owner.length < count ?
                        old->owner.length : count;
    memcpy(replacement + 1, ptr, take);
    host->release(host->context, old);
    return replacement + 1;
}
