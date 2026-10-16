#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy production C bodies verbatim into the native fault-injection fixtures."""
from pathlib import Path
import sys

src, out = map(Path, sys.argv[1:])


def function(code, signature):
    start = code.index(signature)
    return code[start:code.index('\n}\n', start) + 3]


def declaration(code, signature):
    start = code.index(signature)
    return code[start:code.index('\n};', start) + 3] + '\n'


def define(code, name):
    start = code.index('#define ' + name + ' ')
    return code[start:code.index('\n', start) + 1]


wsi = (src/'src/vulkan/wsi/wsi_switch.c').read_text()
(out/'wsi_switch_queue_present.inc').write_text(function(wsi, 'static VkResult\nwsi_switch_swapchain_queue_present('))
md = (src/'src/nouveau/vulkan/nvkmd/nvkmd.h').read_text()
(out/'arena_enums.inc').write_text(declaration(md, 'enum nvkmd_mem_flags {') + declaration(md, 'enum nvkmd_mem_map_flags {'))
(out/'arena_cacheable.inc').write_text(function(md, 'static inline bool\nnvkmd_mem_is_cpu_cacheable('))
arena_h = (src/'src/nouveau/vulkan/nvk_mem_arena.h').read_text()
start = arena_h.index('#define NVK_MEM_ARENA_MIN_SIZE_LOG2')
end = arena_h.index('/** A growable pool of GPU memory')
(out/'arena_helpers.inc').write_text(arena_h[start:end] + declaration(arena_h, 'struct nvk_mem_arena {'))
arena_c = (src/'src/nouveau/vulkan/nvk_mem_arena.c').read_text()
(out/'arena_functions.inc').write_text(
    function(arena_h, 'static inline void\nnvk_contiguous_mem_arena_set_map_dirty_offset(') +
    function(arena_c, 'static void\nnvk_mem_arena_flush_map_locked(') +
    function(arena_c, 'void\nnvk_mem_arena_set_map_dirty(') +
    function(arena_c, 'void\nnvk_mem_arena_copy_to_gpu('))
cache = (src/'src/util/disk_cache_horizon.c').read_text()
(out/'cache_structs.inc').write_text(''.join(declaration(cache, 'struct '+name+' {') for name in ('store_header','store_record','store_entry','index_header','store_stats')))
(out/'cache_defines.inc').write_text(''.join(define(cache, name) for name in (
    'STORE_MAGIC', 'STORE_VERSION', 'INDEX_MAGIC', 'INDEX_VERSION',
    'STORE_NAME_HEX', 'SCAN_CHUNK', 'STORE_COMMIT_INTERVAL_B')))
# In production order, so each include site sees its callees already declared.
cache_signatures = (
    'static void\nentry_free(', 'static void\nindex_put(', 'static bool\nstore_commit(',
    'static bool\nstore_publish(', 'static void\nindex_write(', 'static uint64_t\nindex_load(',
    'static uint64_t\nstore_scan(', 'static bool\nstore_load(', 'static bool\nstore_blob_equal(',
    'static void\nblob_put(', 'static signed long\nblob_get(')
# The duplicate-blob test drives put/get directly and keeps the index itself.
cache_put_only = ('static bool\nstore_commit(', 'static bool\nstore_publish(',
                  'static bool\nstore_blob_equal(', 'static void\nblob_put(',
                  'static signed long\nblob_get(')
(out/'cache_functions.inc').write_text(''.join(
    function(cache, signature) for signature in cache_signatures
    if signature in cache_put_only))
(out/'cache_load.inc').write_text(''.join(
    function(cache, signature) for signature in cache_signatures))
