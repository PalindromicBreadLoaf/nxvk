/*
 * Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nvkmd_nvgpu.h"

#include "util/bitscan.h"
#include "util/cache_ops.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>

#include <switch/arm/cache.h>
#include <switch/result.h>

struct mem_cache_entry {
   struct list_head link;
   NvMap nvmap;
   uint32_t align_B;
};

static void
backing_free(NvMap *nvmap)
{
   void *const cpu_addr = nvmap->cpu_addr;
   nvMapClose(nvmap);
   align_free(cpu_addr);
}

static bool
backing_alloc(uint64_t size_B, uint32_t align_B, uint8_t pte_kind,
              bool cpu_cacheable, NvMap *nvmap_out, Result *rc_out)
{
   *rc_out = 0;

   void *cpu_addr = align_malloc(size_B, align_B);
   if (cpu_addr == NULL)
      return false;

   if (cpu_cacheable)
      util_flush_inval_range(cpu_addr, size_B);

   Result rc = nvMapCreate(nvmap_out, cpu_addr, size_B, align_B,
                           (NvKind)pte_kind, cpu_cacheable);
   if (R_FAILED(rc)) {
      align_free(cpu_addr);
      *rc_out = rc;
      return false;
   }

   return true;
}

static void
mem_cache_destroy_list(struct list_head *list)
{
   list_for_each_entry_safe(struct mem_cache_entry, entry, list, link) {
      backing_free(&entry->nvmap);
      FREE(entry);
   }
}

static void
mem_cache_trim_locked(struct nvkmd_nvgpu_dev *dev,
                      uint64_t max_size_B, uint32_t max_count,
                      struct list_head *evicted)
{
   while (dev->mem_cache_size_B > max_size_B ||
          dev->mem_cache_count > max_count) {
      struct mem_cache_entry *entry =
         list_last_entry(&dev->mem_cache, struct mem_cache_entry, link);

      dev->mem_cache_size_B -= entry->nvmap.size;
      dev->mem_cache_count--;
      list_del(&entry->link);
      list_addtail(&entry->link, evicted);
   }
}

void
nvkmd_nvgpu_mem_cache_trim(struct nvkmd_nvgpu_dev *dev)
{
   struct list_head evicted;
   list_inithead(&evicted);

   simple_mtx_lock(&dev->mem_cache_mutex);
   mem_cache_trim_locked(dev, 0, 0, &evicted);
   simple_mtx_unlock(&dev->mem_cache_mutex);

   mem_cache_destroy_list(&evicted);
}

static bool
mem_cache_take(struct nvkmd_nvgpu_dev *dev,
               uint64_t size_B, uint32_t align_B, uint8_t pte_kind,
               bool cpu_cacheable, NvMap *nvmap_out)
{
   bool hit = false;

   simple_mtx_lock(&dev->mem_cache_mutex);
   list_for_each_entry(struct mem_cache_entry, entry, &dev->mem_cache, link) {
      if (entry->nvmap.size != size_B ||
          entry->nvmap.kind != (NvKind)pte_kind ||
          entry->nvmap.is_cpu_cacheable != cpu_cacheable ||
          entry->align_B < align_B)
         continue;

      dev->mem_cache_size_B -= entry->nvmap.size;
      dev->mem_cache_count--;
      list_del(&entry->link);
      *nvmap_out = entry->nvmap;
      FREE(entry);
      hit = true;
      break;
   }
   dev->mem_cache_hits += hit;
   dev->mem_cache_misses += !hit;
   simple_mtx_unlock(&dev->mem_cache_mutex);

   return hit;
}

/* Returns false if the store was not taken and remains the caller's to free. */
static bool
mem_cache_put(struct nvkmd_nvgpu_dev *dev, struct nvkmd_nvgpu_mem *mem)
{
   if (mem->published || (mem->base.flags & NVKMD_MEM_SHARED))
      return false;

   if (unlikely(dev->base.pdev->debug_flags & NVK_DEBUG_NO_MEM_CACHE))
      return false;

   if (mem->nvmap.size > NVKMD_NVGPU_MEM_CACHE_MAX_B)
      return false;

   struct mem_cache_entry *entry = MALLOC_STRUCT(mem_cache_entry);
   if (entry == NULL)
      return false;

   if (mem->nvmap.is_cpu_cacheable)
      util_flush_inval_range(mem->nvmap.cpu_addr, mem->nvmap.size);

   entry->nvmap = mem->nvmap;
   entry->align_B = mem->base.bind_align_B;

   struct list_head evicted;
   list_inithead(&evicted);

   simple_mtx_lock(&dev->mem_cache_mutex);
   list_add(&entry->link, &dev->mem_cache);
   dev->mem_cache_size_B += entry->nvmap.size;
   dev->mem_cache_count++;
   mem_cache_trim_locked(dev, NVKMD_NVGPU_MEM_CACHE_MAX_B,
                         NVKMD_NVGPU_MEM_CACHE_MAX_ENTRIES, &evicted);
   simple_mtx_unlock(&dev->mem_cache_mutex);

   mem_cache_destroy_list(&evicted);

   return true;
}

VkResult
nvkmd_nvgpu_alloc_mem(struct nvkmd_dev *dev,
                      struct vk_object_base *log_obj,
                      uint64_t size_B, uint64_t align_B,
                      enum nvkmd_mem_flags flags,
                      struct nvkmd_mem **mem_out)
{
   return nvkmd_nvgpu_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                      0 /* pte_kind */, 0 /* tile_mode */,
                                      flags, mem_out);
}

static VkResult
create_mem_or_close_nvmap(struct nvkmd_nvgpu_dev *dev,
                          struct vk_object_base *log_obj,
                          enum nvkmd_mem_flags mem_flags,
                          NvMap *nvmap,
                          enum nvkmd_va_flags va_flags,
                          uint8_t pte_kind, uint64_t va_align_B,
                          uint32_t bind_align_B, bool big_page,
                          struct nvkmd_mem **mem_out)
{
   const uint64_t size_B = nvmap->size;
   VkResult result;

   struct nvkmd_nvgpu_mem *mem = CALLOC_STRUCT(nvkmd_nvgpu_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_nvmap;
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_nvgpu_mem_ops,
                  mem_flags, size_B, bind_align_B);
   mem->nvmap = *nvmap;

   if (big_page) {
      result = nvkmd_nvgpu_alloc_va_ex(&dev->base, log_obj,
                                       va_flags, pte_kind,
                                       size_B, va_align_B,
                                       0 /* fixed_addr */, true /* big_page */,
                                       &mem->base.va);
   } else {
      result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                                  va_flags, pte_kind,
                                  size_B, va_align_B,
                                  0 /* fixed_addr */,
                                  &mem->base.va);
   }
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0 /* va_offset_B */,
                              &mem->base, 0 /* mem_offset_B */, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;

   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   FREE(mem);
fail_nvmap:
   backing_free(nvmap);

   return result;
}

VkResult
nvkmd_nvgpu_alloc_tiled_mem(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            uint64_t size_B, uint64_t align_B,
                            uint8_t pte_kind, uint16_t tile_mode,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_dev);

   /* LOCAL/GART/VRAM placement does not change the allocation. */
   assert(util_bitcount(flags & (NVKMD_MEM_LOCAL |
                                 NVKMD_MEM_GART |
                                 NVKMD_MEM_VRAM)) == 1);

   const bool big_page = (flags & NVKMD_MEM_COMPRESSED) &&
                         dev->big_page_size_B > 0;

   const uint32_t mem_align_B =
      big_page ? (uint32_t)dev->big_page_size_B : _dev->pdev->bind_align_B;
   size_B = align64(size_B, mem_align_B);

   if (size_B > UINT32_MAX) {
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "allocation of 0x%" PRIx64 " bytes is too large",
                       size_B);
   }

   assert(util_is_power_of_two_or_zero64(align_B));
   const uint64_t va_align_B = MAX2(mem_align_B, align_B);

   if (_dev->pdev->debug_flags & NVK_DEBUG_FORCE_COHERENT)
      flags |= NVKMD_MEM_COHERENT;

   /* The GM20B is not IO-coherent. A coherent map is made uncached by
    * nvMapCreate().
    */
   const bool is_cpu_cacheable = !(flags & NVKMD_MEM_COHERENT);

   if (!is_cpu_cacheable || (_dev->pdev->debug_flags & NVK_DEBUG_GPU_UNCACHED))
      flags |= NVKMD_MEM_GPU_UNCACHED;

   NvMap nvmap;
   if (!mem_cache_take(dev, size_B, mem_align_B, pte_kind, is_cpu_cacheable,
                       &nvmap)) {
      Result rc;
      if (!backing_alloc(size_B, mem_align_B, pte_kind, is_cpu_cacheable,
                         &nvmap, &rc)) {
         nvkmd_nvgpu_mem_cache_trim(dev);
         if (!backing_alloc(size_B, mem_align_B, pte_kind, is_cpu_cacheable,
                            &nvmap, &rc)) {
            if (R_FAILED(rc)) {
               return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                "nvMapCreate() failed: 0x%x", (unsigned)rc);
            }
            return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY, "%m");
         }
      }
   }

   return create_mem_or_close_nvmap(dev, log_obj, flags, &nvmap,
                                    0 /* va_flags */, pte_kind, va_align_B,
                                    mem_align_B, big_page, mem_out);
}

VkResult
nvkmd_nvgpu_import_dma_buf(struct nvkmd_dev *_dev,
                           struct vk_object_base *log_obj,
                           int fd, struct nvkmd_mem **mem_out)
{
   /* dma-buf import is unused on the Switch. */
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

void
nvkmd_nvgpu_mem_release(struct nvkmd_nvgpu_mem *mem)
{
   backing_free(&mem->nvmap);
   FREE(mem);
}

static void
nvkmd_nvgpu_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_nvgpu_dev *dev = nvkmd_nvgpu_dev(_mem->dev);
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   nvkmd_va_free(mem->base.va);

   if (!mem_cache_put(dev, mem))
      backing_free(&mem->nvmap);

   FREE(mem);
}

static VkResult
nvkmd_nvgpu_mem_map(struct nvkmd_mem *_mem,
                    struct vk_object_base *log_obj,
                    enum nvkmd_mem_map_flags map_flags,
                    void *fixed_addr,
                    void **map_out)
{
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   /* The CPU pointer is fixed at allocation time, so a fixed-address request
    * can only succeed if it happens to match.
    */
   void *map = nvMapGetCpuAddr(&mem->nvmap);
   if ((map_flags & NVKMD_MEM_MAP_FIXED) && fixed_addr != map)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   *map_out = map;

   return VK_SUCCESS;
}

static void
nvkmd_nvgpu_mem_unmap(struct nvkmd_mem *_mem,
                      enum nvkmd_mem_map_flags flags,
                      void *map)
{
   /* The CPU mapping lives for the lifetime of the nvmap. */
}

static VkResult
nvkmd_nvgpu_mem_overmap(struct nvkmd_mem *_mem,
                        struct vk_object_base *log_obj,
                        enum nvkmd_mem_map_flags flags,
                        void *map)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

/* Only reached if util_has_cache_ops() is false, which should not happen on
 * aarch64.
 */
static void
nvkmd_nvgpu_mem_sync_to_gpu(struct nvkmd_mem *_mem,
                            uint64_t offset_B, uint64_t range_B)
{
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   armDCacheClean((uint8_t *)nvMapGetCpuAddr(&mem->nvmap) + offset_B, range_B);
}

static void
nvkmd_nvgpu_mem_sync_from_gpu(struct nvkmd_mem *_mem,
                              uint64_t offset_B, uint64_t range_B)
{
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   armDCacheFlush((uint8_t *)nvMapGetCpuAddr(&mem->nvmap) + offset_B, range_B);
}

static VkResult
nvkmd_nvgpu_mem_export_dma_buf(struct nvkmd_mem *_mem,
                               struct vk_object_base *log_obj,
                               int *fd_out)
{
   return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);
}

static uint32_t
nvkmd_nvgpu_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nvkmd_nvgpu_mem(_mem)->nvmap.handle;
}

static bool
nvkmd_nvgpu_mem_get_scanout_ids(struct nvkmd_mem *_mem,
                                uint32_t *id_out, uint32_t *handle_out)
{
   struct nvkmd_nvgpu_mem *mem = nvkmd_nvgpu_mem(_mem);

   mem->published = true;
   *id_out = mem->nvmap.id;
   *handle_out = mem->nvmap.handle;
   return true;
}

const struct nvkmd_mem_ops nvkmd_nvgpu_mem_ops = {
   .free = nvkmd_nvgpu_mem_free,
   .map = nvkmd_nvgpu_mem_map,
   .unmap = nvkmd_nvgpu_mem_unmap,
   .overmap = nvkmd_nvgpu_mem_overmap,
   .sync_to_gpu = nvkmd_nvgpu_mem_sync_to_gpu,
   .sync_from_gpu = nvkmd_nvgpu_mem_sync_from_gpu,
   .export_dma_buf = nvkmd_nvgpu_mem_export_dma_buf,
   .log_handle = nvkmd_nvgpu_mem_log_handle,
   .get_scanout_ids = nvkmd_nvgpu_mem_get_scanout_ids,
};
