/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host syntax checks only. Not a libnx ABI/runtime implementation. */
#ifndef TEST_SYNTAX_SWITCH_H
#define TEST_SYNTAX_SWITCH_H
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
#include <switch/result.h>
#define FS_MAX_PATH 769
#define InfoType_TotalMemorySize 6
#define AppletType_Application 0
#define HidNpadStyleSet_NpadStandard 0
#define HidNpadButton_Plus 1
#define HOSVER_MAJOR(v) (((v) >> 16) & 255)
#define HOSVER_MINOR(v) (((v) >> 8) & 255)
#define HOSVER_MICRO(v) ((v) & 255)
extern struct in_addr __nxlink_host;
typedef struct { int unused; } PadState;
typedef struct { int unused; } ClkrstSession;
typedef enum { PcvModule_CpuBus, PcvModule_GPU, PcvModule_EMC } PcvModule;
typedef enum { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC } PcvModuleId;
Result socketInitializeDefault(void);
void socketExit(void);
int nxlinkStdio(void);
u32 hosversionGet(void);
bool hosversionAtLeast(unsigned, unsigned, unsigned);
u32 appletGetOperationMode(void);
u32 appletGetPerformanceMode(void);
bool appletMainLoop(void);
void consoleInit(void *);
void consoleUpdate(void *);
void consoleExit(void *);
void padConfigureInput(unsigned, unsigned);
void padInitializeDefault(PadState *);
void padUpdate(PadState *);
u64 padGetButtonsDown(PadState *);
bool envHasNextLoad(void);
Result envSetNextLoad(const char *, const char *);
Result clkrstInitialize(void);
void clkrstExit(void);
Result clkrstOpenSession(ClkrstSession *, PcvModuleId, u32);
void clkrstCloseSession(ClkrstSession *);
Result clkrstGetClockRate(ClkrstSession *, u32 *);
Result pcvInitialize(void);
void pcvExit(void);
Result pcvGetClockRate(PcvModule, u32 *);
#endif
