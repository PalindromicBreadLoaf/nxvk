# Bringing a real application onto this driver

An external project — an emulator, a game, a port — consumes this driver as a
portlib package, not as a Mesa build tree. This is what to take, how to link it,
what to run before your own code, and what to log so that "it helped" can be
attributed to a driver revision instead of a feeling.

## Take a pinned package, not a branch head

Every push to a development branch runs the Vulkan-only workflow, which builds
the driver, links the 22-app ladder against it and publishes
`nxvk-vulkan-tegra-x1-<run-number>`. That artifact contains:

- `nxvk-vulkan-tegra-x1.tar.gz` — the package: `lib/libnvk.a`,
  `lib/libnvk_support.a`, `lib/pkgconfig/nxvk.pc`, `include/vulkan`,
  `include/vk_video`, `licenses/nxvk/`, and `BUILDINFO.txt`,
  `TEGRA-X1-AUDIT.md`, `TEGRA-X1-OPTIMIZATION.md` for the record;
- `nxvk-vulkan-tegra-x1.tar.gz.sha256`;
- `package-check/nvk_smoke.nro` — the same package, linked into a runnable
  smoke test (see below).

```sh
tar -xzf nxvk-vulkan-tegra-x1.tar.gz -C "$DEVKITPRO/portlibs/switch"
# or extract anywhere and point pkg-config at it:
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$PWD/portlib/lib/pkgconfig"
```

Record the tarball's sha256 and the workflow run URL in your build metadata.
`BUILDINFO.txt` inside the package names the source commit, the CI run, the
toolchain image and one `Linked <archive> SHA256:` line per archive the driver
was built from (18 of them); the sha256 of that file is the build ID the driver
prints in its own logs. A comparison without that ID is not attributable.

CI verifies this package the way a consumer sees it: `verify-package.sh`
extracts it into a clean prefix, resolves it through an isolated
`pkg-config` and compiles/links a client against it, so an out-of-tree project
linking this package is a path that is exercised on every push.

## Run the packaged smoke test first

`package-check/nvk_smoke.nro` links the same package and exercises bring-up,
memory, a submit and a readback, writing `sdmc:/nvk_smoke.log`. If it fails, the
problem is the driver or the console state, not your integration. Keep it on the
SD card next to your build.

## Linking

```sh
# after installing into $(DEVKITPRO)/portlibs/switch
$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config --cflags --libs nxvk
# or, from an extracted package elsewhere
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR=<prefix>/lib/pkgconfig pkg-config --cflags --libs nxvk
```

expands to

```
-I<prefix>/include -D__SWITCH__ -DVK_USE_PLATFORM_VI_NN -ffunction-sections -fdata-sections
-L<prefix>/lib -Wl,--whole-archive -lnvk -Wl,--no-whole-archive
-Wl,--start-group -lnvk_support -lz -lexpat -Wl,--end-group
-Wl,-u,vk_icdGetInstanceProcAddr -Wl,--gc-sections
```

- `libnvk.a` is the driver, so it is linked with `--whole-archive`, and
  `-u,vk_icdGetInstanceProcAddr` keeps the ICD entry point in the link.
- There is no system Vulkan loader on Horizon, and `libnvk.a` defines **only**
  `vk_icdGetInstanceProcAddr` — no plain `vkGetInstanceProcAddr`. Resolve
  instance functions through the ICD; if your renderer calls
  `vkGetInstanceProcAddr` directly, add a one-line shim:

```c
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
   return vk_icdGetInstanceProcAddr(instance, name);
}
```

- Keep `-ffunction-sections -fdata-sections` on your own objects together with
  the package's `--gc-sections`: the driver is large, and this is what keeps the
  entry points you never resolve out of your NRO.

## What the application must declare

```c
u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

    /* Before creating the instance. */
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
```

- A full-memory application, not an applet: an applet gets less memory and fewer
  cores, and the driver needs the memory.
- The environment variable is **not optional**. On a device NVK is not
  conformant for, `nvk_physical_device_create()` returns
  `VK_ERROR_INCOMPATIBLE_DRIVER` unless it is set — in a release build that is
  the whole symptom, with no message and no device to enumerate.
- The process then gets the core mask Horizon grants it (`0x7`, three cores on a
  full-memory application), which is what Mesa sizes itself from.

## Presentation

- Enable `VK_KHR_surface` and `VK_NN_vi_surface` on the instance and
  `VK_KHR_swapchain` on the device; create the surface with
  `vkCreateViSurfaceNN()` over your `NWindow` (or `nwindowGetDefault()`), and
  use `minImageCount = 3`.
- Present modes: FIFO, MAILBOX, IMMEDIATE.
- Scanout is zero-copy only for a single-plane image whose `pte_kind` is `0xfe`.
  Otherwise the driver swizzles the whole image on the CPU per present and
  flushes the data cache (~3.7 MB at 720p). `NVK_DEBUG=wsi` prints which path a
  swapchain uses at creation (`nvk wsi: zero-copy ENABLED`) and its totals at
  teardown:

```
nvk wsi: N frames, N syncpt handoffs, N fence blocks, N dequeues without a
release wait, N release waits, N us in swizzle+flush
```

That line is how a real application measures the largest per-frame CPU cost the
[source audit](tegra-x1-audit.md) identified. The 22-app ladder never exercised
it: both of its presenting apps run zero-copy and spend their time waiting for
vsync.

## What to log for an attributable report

Set these before instance creation for the run you want to discuss:

| Variable | What it gives you |
| --- | --- |
| `MESA_LOG_FILE=sdmc:/<app>-mesa.log` | Mesa's own log; without it Mesa's messages go to stderr, which on Horizon is usually nowhere |
| `MESA_SHADER_CACHE_SHOW_STATS=1` | the store's open line (entries, size) and close line (blobs, appends, duplicates, header publications) |
| `NVK_DEBUG=wsi` | the per-swapchain present statistics quoted above |
| `NVK_DEBUG=vm` | arena flush counters |
| `MESA_SHADER_CACHE_COMMIT=per-blob` | same-build A/B of the batched header publication |

Worth sending back, in this order:

1. the package sha256, the workflow run URL, and the build ID in your log;
2. the Mesa log above;
3. the store's close line — for a shader-heavy application this is where the
   counts get interesting. The ladder appends 2-6 blobs per app and publishes
   once; an emulator compiles thousands of shaders, which is the workload the
   batched publication was written for;
4. whether the present path was zero-copy, and the `swizzle+flush` total if it
   was not;
5. a frame-time split that separates the application's own threads from driver
   calls — if only "FPS before/after" exists, say what else changed between the
   two builds.

## What not to expect

- The ladder's numbers do not predict an emulator. Its rendering tests are 4-9 %
  busy on the main thread and its presenting apps are vsync-bound; a PS2
  emulator spends its CPU time in its own recompilers. "The driver helped" has
  to be argued from a measurement that separates the two.
- On a cold shader cache, expect pipeline creation to be dominated by shader
  compilation on the application's thread: NAK runs inside
  `vkCreateGraphicsPipelines`. The pipeline cache is thread-safe, so an
  application can compile on several threads; the driver does not serialize it.
- Nothing here promises a frame rate.
