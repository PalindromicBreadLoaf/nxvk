/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Stub of <sys/mman.h> for the cross target.
 */
#ifndef NXVK_STUB_SYS_MMAN_H
#define NXVK_STUB_SYS_MMAN_H

#include <sys/types.h>
#include <stddef.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

#define MS_ASYNC      0x1
#define MS_INVALIDATE 0x2
#define MS_SYNC       0x4

#ifdef __cplusplus
extern "C" {
#endif

static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, off_t offset)
{
   (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
   return MAP_FAILED;
}

static inline int munmap(void *addr, size_t length)
{
   (void)addr; (void)length;
   return -1;
}

static inline int mprotect(void *addr, size_t len, int prot)
{
   (void)addr; (void)len; (void)prot;
   return -1;
}

static inline int msync(void *addr, size_t length, int flags)
{
   (void)addr; (void)length; (void)flags;
   return -1;
}

static inline int madvise(void *addr, size_t length, int advice)
{
   (void)addr; (void)length; (void)advice;
   return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* NXVK_STUB_SYS_MMAN_H */
