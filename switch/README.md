# NXVK
(NVK + NX = NXVK? NXNVK? NVKNX?)

A subset of the **Mesa** project that ports the [NVK](https://docs.mesa3d.org/drivers/nvk.html) Vulkan driver to the Nintendo Switch.

Currently, this is based on **Mesa 26.1.4**.

## Licensing

Upstream Mesa code retains its existing licences. All new files added by this fork are MIT-licensed
This fork does **not** copy any GPL-licensed source from the `switch-nvk` project.
Hardware behaviour was learned from its documentation and then reimplemented by hand.

## Building

A container solution capable of running docker images is required to build this. You can do it without,
but I won't be helping with that.

This was verified on Fedora Linux using podman. I hope it works elsewhere.

Build the toolchain image once (from repo root)

```shell
 [podman | docker | whatever] build -t nvk-switch-build switch/docker
```

One-time native tooling

```shell
 cd nxvk
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work localhost/nvk-switch-build bash -lc '
     bash switch/build/build-native-tools.sh &&
     bash switch/build/configure-mesa.sh'
```

Build the driver archives and package the validation `.nro`s:

```shell
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work localhost/nvk-switch-build bash -lc '
     ninja -k0 -C /work/switch/build/cross src/nouveau/vulkan/libvulkan_nouveau.so || true
     for app in nvk_smoke nvk_tri nvk_logo nvk_scene nvk_indexed \
                nvk_multi nvk_textures nvk_cubemap nvk_vi_swapchain; do
       bash switch/build/build-nro.sh $app || exit 1
     done'
```

The final `libvulkan_nouveau.so` link will fail. This is expected.
The `|| true` is expected and forces every `lib*.a` archive to build regardless.

## Usage

You must link the driver's static archives directly into your `.nro` and call
it through its single exported entrypoint: `vk_icdGetInstanceProcAddr`.

### What you link against

After a build, the driver is the archive set under `switch/build/cross`:

- `src/nouveau/vulkan/libnvk.a` is the driver itself.
- the support archives it depends on (NAK, NIL, NIR, SPIR-V/`vtn`, `vulkan_util`, `mesa_util`, …) — the
  complete list is in the ARCHIVES variable in `switch/build/build-nro.sh`.
- devkitPro portlibs `libz` + `libexpat`

### Linking

`switch/build/build-nro.sh` is the provided reference linker. To build your own app, drop your .c file(s) next to the
smoke apps in `switch/smoke/` and run `build-nro.sh` on it to test linkage.
For a proper link, copy its link line into your own build.

Be sure to include the following linker flags in your project else this won't work.
- `-Wl,--whole-archive`
- `-Wl,-u,vk_icdGetInstanceProcAddr`
- `-Wl,--gc-sections` together with `-ffunction-sections -fdata-sections` to drop dead code.
- Compile with `-D__SWITCH__ -DVK_USE_PLATFORM_VI_NN`, and include Mesa's vendored Vulkan headers with
  `-I<repo>/include` alongside `-I$DEVKITPRO/libnx/include`.
- Also link `switch/smoke/nvk_compat.c` Whilst not strictly required, it supplies the newlib gaps the driver references for your convenience.
  (`getrandom`, `posix_memalign`, `sysconf`, `get*id`, `regcomp`/`regexec`/`regfree`).
- Link against libnx with `-specs=$DEVKITPRO/libnx/switch.specs`.

### Calling

Everything else is resolved through one symbol.

```c
#include <switch.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

extern PFN_vkVoidFunction
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* This is required to not OOM immediately. */
u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

int main(void) {
    /* NVK refuses device creation without this. */
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);

    PFN_vkCreateInstance CreateInstance =
        (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
    /* create the instance, then resolve instance-level functions via
       vk_icdGetInstanceProcAddr(instance, "vk..."), and device-level functions
       via vkGetDeviceProcAddr(device, "vk..."). */
}
```

- **Instance-level** functions: `vk_icdGetInstanceProcAddr(instance, "vkFoo")`.
- **Device-level** functions: resolve `vkGetDeviceProcAddr` through the ICD once, then
  `vkGetDeviceProcAddr(device, "vkFoo")`.
- **Presenting to the screen**: enable the `VK_KHR_surface` + `VK_NN_vi_surface` instance extensions, and
  create the surface with `vkCreateViSurfaceNN` over a libnx `NWindow`. Swapchain images scan out zero-copy
  (block-linear, `kind=0xfe`); triple-buffer with `minImageCount = 3`.

### Or... skip all of that

`switch/smoke/nvk_harness.h` already implements the entire above thing. Include it and call
`nvk_bringup(&ctx, dev_exts, n)` (or `nvk_bringup_ex(...)` when you need WSI instance extensions) to get a
ready `VkInstance` / `VkPhysicalDevice` / `VkDevice` / `VkQueue`, plus the `LOAD_INST` / `LOAD_DEV`
entrypoint-resolution macros (and debug messenging all wired up too). See the nine `nvk_*.c` apps in `switch/smoke/`
are examples of how to do all of this, from a headless fill-and-readback up to a WSI swapchain present.

## Layout

The `switch/` directory contains all Switch-specific build scripts, toolchain, docs, and whatever else is needed. 
This is to both allow for easier rebasing on newer Mesa versions, and to provide an easy place to manage all changes for this port.

## Contributing

All contributions are welcome, however, prior to doing any work I recommend opening an issue to discuss what you wish to do
so that it aligns with the project's scope.
