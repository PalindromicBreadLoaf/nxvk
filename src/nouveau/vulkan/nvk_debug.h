/*
 * Copyright © 2024 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_DEBUG_H
#define NVK_DEBUG_H 1

enum nvk_debug {
   /* dumps all push buffers after submission */
   NVK_DEBUG_PUSH_DUMP = 1ull << 0,

   /* push buffer submissions wait on completion
    *
    * This is useful to find the submission killing the GPU context. For
    * easier debugging it also dumps the buffer leading to that.
    */
   NVK_DEBUG_PUSH_SYNC = 1ull << 1,

   /* Zero all client memory allocations
    */
   NVK_DEBUG_ZERO_MEMORY = 1ull << 2,

   /* Write repeating nonzero patterns to client memory allocations
    */
   NVK_DEBUG_TRASH_MEMORY = 1ull << 3,

   /* Dump VM bind/unbinds
    */
   NVK_DEBUG_VM = 1ull << 4,

   /* Disable most cbufs
    *
    * Root descriptors still end up in a cbuf
    */
   NVK_DEBUG_NO_CBUF = 1ull << 5,

   /* Use the EXT_descriptor_buffer path for all buffer views */
   NVK_DEBUG_FORCE_EDB_BVIEW = 1ull << 6,

   /* Force all memory allocations to go to GART */
   NVK_DEBUG_FORCE_GART = 1ull << 7,

   /* Force all memory allocations to go to GART */
   NVK_DEBUG_FORCE_COHERENT = 1ull << 8,

   /* Disable image compression */
   NVK_DEBUG_NO_COMPRESSION = 1ull << 9,

   /* Log vk_error*() messages.
    *
    * A release build drops them unless the instance opts into debug logging
    */
   NVK_DEBUG_ERRORS = 1ull << 10,

   /* Map every allocation GPU-uncached */
   NVK_DEBUG_GPU_UNCACHED = 1ull << 11,

   /* Service GPU waits with a CPU block instead of an engine wait */
   NVK_DEBUG_CPU_WAIT = 1ull << 12,

   /* Tear down every memory allocation instead of recycling its backing store */
   NVK_DEBUG_NO_MEM_CACHE = 1ull << 13,

   /* Flush whole mem arenas on submit instead of only their dirty ranges */
   NVK_DEBUG_FULL_ARENA_FLUSH = 1ull << 14,

   /* Log the WSI present path and its per-frame counters */
   NVK_DEBUG_WSI = 1ull << 15,

   /* Block the CPU on the render fence and dequeue through libnx. */
   NVK_DEBUG_CPU_PRESENT = 1ull << 16,

   /* Flush whole command buffer mems on end. */
   NVK_DEBUG_FULL_CMD_FLUSH = 1ull << 17,

   /* Run copies through vk_meta instead of the copy engine */
   NVK_DEBUG_META_COPY = 1ull << 18,

   /* Fill the channel warmup ramp with fence cmdlists rather than no-ops */
   NVK_DEBUG_DENSE_WARMUP = 1ull << 19,

   /* Issue one copy engine launch per line instead of one per copy */
   NVK_DEBUG_SPLIT_CE_COPY = 1ull << 20,

   /* Do not promote block-linear sector fetches on texture headers */
   NVK_DEBUG_NO_SECTOR_PROMOTION = 1ull << 21,
};

enum nvk_experimental {
   /* Enable dlss support */
   NVK_EXPERIMENTAL_DLSS = 1ull << 0,

   /* Enable dlss backwards compat
    *
    * Allow using a SASS binary with a matching major version number, but
    * smaller minor number than the device.
    */
   NVK_EXPERIMENTAL_DLSS_BACK_COMPAT = 1ull << 1,
};

#endif /* NVK_DEBUG_H */
