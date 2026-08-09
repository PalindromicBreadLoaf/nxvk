/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Stub of <syslog.h> for the cross target.
 */
#ifndef NXVK_STUB_SYSLOG_H
#define NXVK_STUB_SYSLOG_H

#include <stdarg.h>

#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

#define LOG_PID    0x01
#define LOG_CONS   0x02
#define LOG_ODELAY 0x04
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

#define LOG_KERN   (0 << 3)
#define LOG_USER   (1 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_LOCAL0 (16 << 3)

#ifdef __cplusplus
extern "C" {
#endif

static inline void openlog(const char *ident, int option, int facility)
{
   (void)ident; (void)option; (void)facility;
}

static inline void syslog(int priority, const char *format, ...)
{
   (void)priority; (void)format;
}

static inline void closelog(void) {}

#ifdef __cplusplus
}
#endif

#endif /* NXVK_STUB_SYSLOG_H */
