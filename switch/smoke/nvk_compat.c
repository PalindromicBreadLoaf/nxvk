/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Link-time newlib gap fills for the Switch smoke apps.
 */
#include <sys/types.h> /* off_t, before <regex.h> which uses it undeclared */
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <unistd.h>
#include <regex.h>
#include <signal.h>

#include <switch.h>

/* Back rust's getrandom with CSRNG so the values are actually random. */
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
   (void)flags;
   randomGet(buf, buflen);
   return (ssize_t)buflen;
}

/* newlib has memalign() but not posix_memalign(). */
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
   if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
      return EINVAL;
   void *p = memalign(alignment, size);
   if (!p)
      return ENOMEM;
   *memptr = p;
   return 0;
}

uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

/* Enough of sysconf for os_get_total_physical_memory() and page-size queries. */
long sysconf(int name)
{
   switch (name) {
   case _SC_PAGESIZE:         return 4096;
   case _SC_PHYS_PAGES:       return (3ll * 1024 * 1024 * 1024) / 4096;
   case _SC_NPROCESSORS_CONF:
   case _SC_NPROCESSORS_ONLN: return 4;
   default:                   return -1;
   }
}

/* This is not meaningful for homebrew, but needed regardless,
 * so compile is a no-op and every match reports "no match". */
int regcomp(regex_t *preg, const char *regex, int cflags)
{
   (void)regex; (void)cflags;
   if (preg) preg->re_nsub = 0;
   return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags)
{
   (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags;
   return REG_NOMATCH;
}

void regfree(regex_t *preg) { (void)preg; }

/* Gallium's worker threads (u_queue.c) block signals. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
   (void)how; (void)set;
   if (oldset)
      *oldset = 0;
   return 0;
}
