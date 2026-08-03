# NXVK
(NVK + NX = NXVK? NXNVK? NVKNX?)

A subset of the **Mesa** project that ports the [NVK](https://docs.mesa3d.org/drivers/nvk.html) Vulkan driver to the Nintendo Switch.

OpenGL is also supported via Mesa's [Zink](https://docs.mesa3d.org/drivers/zink.html) driver, which runs
GL on top of NVK, so the same build also gives you access to **OpenGL 4.5 core** and **OpenGL ES 3.2**,
with **EGL** over libnx `NWindow` for windowing and presentation.

Currently, this is based on **Mesa 26.1.4**.

## Licensing

Upstream Mesa code retains its existing licences. All new files added by this fork are MIT-licensed.

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

### With OpenGL

The GL stack is a secondary build directory: `switch/build/cross-zink`, which is configured by
`configure-zink.sh` (the same options as configure-mesa plus Zink/Gallium/GL/EGL). It builds NVK too,
so it is a superset, but it is kept separate so that Vulkan-only archives don't gain needless codesize.

```shell
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work localhost/nvk-switch-build bash -lc '
     export PATH=/work/switch/build/native-tools/bin:$PATH
     bash switch/build/configure-zink.sh
     ninja -k0 -C /work/switch/build/cross-zink src/nouveau/vulkan/libvulkan_nouveau.so || true'
```

Then point `build-nro.sh` at it with `CROSS_BUILD`. It links the GL archives only for apps named
`gl_*` or `gles*`.

```shell
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work \
     -e CROSS_BUILD=/work/switch/build/cross-zink \
     localhost/nvk-switch-build bash switch/build/build-nro.sh gl_tri
```

## Vulkan Usage

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

## OpenGL Usage

Same deal as Vulkan really. No loader, no GLVND, no shared libraries, etcetera. Zink, Gallium, the GL frontend and
EGL all link statically into your `.nro`, and Zink finds NVK through the very same `vk_icdGetInstanceProcAddr`
symbol.

Currently, you get OpenGL 4.5 core, a compatibility context, and OpenGL ES 2.0 / 3.2.

### What you link against

Everything from the Vulkan list above (Zink still needs the whole driver), plus, from
`switch/build/cross-zink`:

- `src/egl/libEGL.a`
- `src/mesa/libmesa.a`
- `src/compiler/glsl/libglsl.a`, `src/compiler/glsl/glcpp/libglcpp.a`
- `src/mesa/glapi/shared-glapi/libglapi.a`, `src/mesa/glapi/glapi/libglapi_bridge.a`
- `src/gallium/auxiliary/libgallium.a`, `src/gallium/drivers/zink/libzink.a`
- `src/gallium/winsys/zink/drm/libzinkwinsys.a`
- `src/gallium/winsys/sw/null/libws_null.a`, `src/gallium/winsys/sw/wrapper/libwsw.a`

The `GL_ARCHIVES` variable in `build-nro.sh` is the authoritative copy of that list.
Look for changes there if this list is outdated ever.

### Linking

Everything from the Vulkan linking rules still applies, and in addition:

- The GL archives go inside `--whole-archive` alongside `libnvk.a`.
- Do **not** link `libglsl_util.a`. It multiply-defines `_mesa_error_no_memory` against `libmesa.a`.
- Compile your GL sources with `-DHAVE_PTHREAD` and Mesa's internal include set (see `GL_INC` in `build-nro.sh`)

### Calling

Ordinary EGL, with libnx's default window as the native window:

```c
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>   /* or <GL/gl.h> for desktop GL */

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

int main(void) {
    /* Zink comes up on NVK, which still refuses device creation without this. */
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);      /* or EGL_OPENGL_API */

    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE,
    };
    EGLConfig cfg; EGLint n;
    eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n);

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, nwindowGetDefault(), NULL);

    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE,
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    eglMakeCurrent(dpy, surf, surf, ctx);

    /* Do you draws, then: */
    eglSwapBuffers(dpy, surf);
}
```

### Or... skip all of that (again)

`switch/smoke/gl_egl_harness.h` does the whole bring-up thing already for you. `gl_egl_up(&e, EGL_OPENGL_ES_API, 3, 2)` hands
back a current context on a window surface, and it carries a multi-stage GLSL program builder and
`glReadPixels` probe helpers beside it. The `gl_*` and `gles3` apps in `switch/smoke/` are worked examples, of how to
use both it and GL itself, from a bare `glClear` up to geometry and tessellation shaders. See `switch/smoke/README.md` for the
table and what each test covers.

## Layout

The `switch/` directory contains all Switch-specific build scripts, toolchain, docs, and whatever else is needed. 
This is to both allow for easier rebasing on newer Mesa versions, and to provide an easy place to manage all changes for this port.

## Contributing

All contributions are welcome, however, prior to doing any work I recommend opening an issue to discuss what you wish to do
so that it aligns with the project's scope.

## Support
If you find NXVK useful and would like to support its development, you can [donate to PalindromicBreadLoaf on Ko-fi](https://ko-fi.com/palindromicbreadloaf).
