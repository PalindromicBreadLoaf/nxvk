/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: MIT
 *
 * Stub of <spirv-tools/libspirv.h> for the cross target.
 */
#ifndef NXVK_STUB_SPIRV_TOOLS_LIBSPIRV_H
#define NXVK_STUB_SPIRV_TOOLS_LIBSPIRV_H

#include <stddef.h>
#include <stdint.h>

typedef int spv_target_env;
typedef int spv_result_t;
typedef uint32_t spv_binary_to_text_options_t;

#define SPV_SUCCESS 0
#define SPV_ENV_UNIVERSAL_1_6 0

#define SPV_BINARY_TO_TEXT_OPTION_INDENT         0x1
#define SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES 0x2
#define SPV_BINARY_TO_TEXT_OPTION_COLOR          0x4

typedef struct spv_context_t    *spv_context;
typedef struct spv_diagnostic_t *spv_diagnostic;

typedef struct spv_text_t {
   const char *str;
   size_t      length;
} spv_text_t;
typedef const spv_text_t *spv_text;

#ifdef __cplusplus
extern "C" {
#endif

static inline spv_context spvContextCreate(spv_target_env env)
{
   (void)env;
   return (spv_context)0;
}

static inline void spvContextDestroy(spv_context context)
{
   (void)context;
}

static inline spv_result_t
spvBinaryToText(spv_context context, const uint32_t *binary, size_t word_count,
                spv_binary_to_text_options_t options, spv_text *text,
                spv_diagnostic *diagnostic)
{
   (void)context; (void)binary; (void)word_count; (void)options;
   if (text)       *text = (spv_text)0;
   if (diagnostic) *diagnostic = (spv_diagnostic)0;
   return -1; /* != SPV_SUCCESS */
}

static inline void spvDiagnosticPrint(spv_diagnostic diagnostic)
{
   (void)diagnostic;
}

static inline void spvDiagnosticDestroy(spv_diagnostic diagnostic)
{
   (void)diagnostic;
}

static inline void spvTextDestroy(spv_text text)
{
   (void)text;
}

#ifdef __cplusplus
}
#endif

#endif /* NXVK_STUB_SPIRV_TOOLS_LIBSPIRV_H */
