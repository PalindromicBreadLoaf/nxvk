/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Stub of <dlfcn.h> for the cross target.
 */
#ifndef NXVK_STUB_DLFCN_H
#define NXVK_STUB_DLFCN_H

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100

#ifdef __cplusplus
extern "C" {
#endif

static inline void *dlopen(const char *file, int mode)
{
   (void)file; (void)mode;
   return (void *)0;
}

static inline int dlclose(void *handle)
{
   (void)handle;
   return 0;
}

static inline void *dlsym(void *handle, const char *name)
{
   (void)handle; (void)name;
   return (void *)0;
}

static inline char *dlerror(void)
{
   return (char *)"dynamic loading is unsupported on Horizon";
}

#ifdef __cplusplus
}
#endif

#endif /* NXVK_STUB_DLFCN_H */
