# NXVK
(NVK + NX = NXVK? NXNVK? NVKNX?)

A subset of the **Mesa** project that ports the [NVK](https://docs.mesa3d.org/drivers/nvk.html) Vulkan driver to the Nintendo Switch.

OpenGL is also supported via Mesa's [Zink](https://docs.mesa3d.org/drivers/zink.html) driver, which runs
GL on top of NVK, so the same build also gives you access to **OpenGL 4.5 core** and **OpenGL ES 3.2**,
with **EGL** over libnx `NWindow` for windowing and presentation.

Currently, this is based on **Mesa 26.2.2**.

## Licensing

Upstream Mesa code retains its existing licences. All new files added by this fork are **GPL-2.0-or-later**.

Because Horizon links `libnvk.a` statically, an application linking the driver forms a combined
work covered by the GPL. You may ship binaries freely, however, the application's source must be
available to whoever receives them. Permissively licensed ports (MIT, BSD, etc.) can link it and
keep their own licence on their own source.

## Building

A container solution capable of running docker images is required to build this. You can do it without,
but I won't be helping with that.

This was verified on Fedora Linux using podman. I hope it works elsewhere.

### The easy way

Make runs the native tooling and cross configure, builds the driver archives, and bundles them into
a devkitPro-like portlib you can then install. From the repo root:

```shell
 make image        # (Run once) build the toolchain image
 make              # Build the Vulkan driver and stage the package
 make gl           # Build the OpenGL stack and stage its package
```

Staged output lands in `switch/build/pkg/lib/`: `libnvk.a`, `libnvk_support.a` (and `libnvk_gl.a`
for GL), plus `pkgconfig/nxvk.pc` (and `nxvk-gl.pc`). 
Install it alongside the other devkitPro portlibs (see [Installing](#installing-devkitpro-portlib) below),
or link straight from there (not recommended).

Targets:

| Target | What it does |
|---|---|
| `make` / `make gl` | build and stage the Vulkan / Vulkan+GL package |
| `sudo make install` / `sudo make install-gl` | copy the package into `$DEVKITPRO/portlibs/switch` |
| `make nro APP=nvk_smoke` | link a smoke app into `switch/smoke/out/<app>.nro` (for testing) |
| `make uninstall` | remove the installed nxvk files |
| `make clean` / `make distclean` | drop the cross builds / native tools |

### The hard way

The Makefile really just chains the scripts in `switch/build/`. To do it yourself:

Build the toolchain image once (from repo root)

```shell
 [podman | docker | whatever] build -t nxvk switch/docker
```

One-time native tooling

```shell
 cd nxvk
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work localhost/nxvk bash -lc '
     bash switch/build/build-native-tools.sh &&
     bash switch/build/configure-mesa.sh'
```

Build the driver archives and package the validation `.nro`s:

```shell
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work localhost/nxvk bash -lc '
     ninja -k0 -C /work/switch/build/cross src/nouveau/vulkan/libvulkan_nouveau.so || true
     for app in nvk_smoke nvk_tri nvk_logo nvk_scene nvk_indexed \
                nvk_multi nvk_textures nvk_cubemap nvk_vi_swapchain; do
       bash switch/build/build-nro.sh $app || exit 1
     done'
```

See [`switch/smoke/README.md`](smoke/README.md) for usage of the validation suit.

The final `libvulkan_nouveau.so` link will fail. This is expected.
The `|| true` forces every `lib*.a` archive to build regardless.

#### With OpenGL

The GL stack is a secondary build directory: `switch/build/cross-zink`, which is configured by
`configure-zink.sh` (the same options as configure-mesa plus Zink/Gallium/GL/EGL). It builds NVK too,
so it is a superset, but it is kept separate so that Vulkan-only archives don't gain needless codesize.

```shell
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work localhost/nxvk bash -lc '
     export PATH=/work/switch/build/native-tools/bin:$PATH
     bash switch/build/configure-zink.sh
     ninja -k0 -C /work/switch/build/cross-zink src/nouveau/vulkan/libvulkan_nouveau.so || true'
```

Then point `build-nro.sh` at it with `CROSS_BUILD`. It links the GL archives only for apps named
`gl_*` or `gles*`.

```shell
 [podman | docker | whatever] run --rm -v "$PWD:/work:z" -w /work \
     -e CROSS_BUILD=/work/switch/build/cross-zink \
     localhost/nxvk bash switch/build/build-nro.sh gl_tri
```

## Installing

`sudo make install` drops the package into `$DEVKITPRO/portlibs/switch`, next to the other devkitPro
libraries:

- `lib/libnvk.a`, `lib/libnvk_support.a`  (and `lib/libnvk_gl.a` with `install-gl`)
- `lib/pkgconfig/nxvk.pc`  (and `nxvk-gl.pc`)
- `include/vulkan/`, `include/vk_video/`
- `licenses/nxvk/`

## Vulkan support

The supported Vulkan version is 1.3, with 1.4 being prevented by requiring
`VK_EXT_host_image_copy`, which NVK only implements on Turing and later. 

Below is the full list of supported Vulkan extensions:
(Note that it may be easier to look at what isn't supported first)

### Instance extensions

`VK_KHR_surface`, `VK_KHR_get_surface_capabilities2`, `VK_KHR_surface_maintenance1`,
`VK_KHR_surface_protected_capabilities`, `VK_EXT_surface_maintenance1`,
`VK_EXT_swapchain_colorspace`, `VK_EXT_headless_surface`, `VK_NN_vi_surface`,
`VK_KHR_device_group_creation`, `VK_KHR_get_physical_device_properties2`,
`VK_KHR_external_fence_capabilities`, `VK_KHR_external_memory_capabilities`,
`VK_KHR_external_semaphore_capabilities`, `VK_EXT_debug_report`, `VK_EXT_debug_utils`

### Device extensions (177)

<details>
<summary>The full list</summary>

**KHR (88)**: `VK_KHR_8bit_storage`, `VK_KHR_16bit_storage`, `VK_KHR_bind_memory2`, `VK_KHR_buffer_device_address`, `VK_KHR_calibrated_timestamps`, `VK_KHR_copy_commands2`, `VK_KHR_copy_memory_indirect`, `VK_KHR_create_renderpass2`, `VK_KHR_dedicated_allocation`, `VK_KHR_depth_clamp_zero_one`, `VK_KHR_depth_stencil_resolve`, `VK_KHR_descriptor_update_template`, `VK_KHR_device_group`, `VK_KHR_driver_properties`, `VK_KHR_dynamic_rendering`, `VK_KHR_dynamic_rendering_local_read`, `VK_KHR_external_fence`, `VK_KHR_external_fence_fd`, `VK_KHR_external_memory`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore`, `VK_KHR_external_semaphore_fd`, `VK_KHR_format_feature_flags2`, `VK_KHR_get_memory_requirements2`, `VK_KHR_global_priority`, `VK_KHR_image_format_list`, `VK_KHR_imageless_framebuffer`, `VK_KHR_incremental_present`, `VK_KHR_index_type_uint8`, `VK_KHR_line_rasterization`, `VK_KHR_load_store_op_none`, `VK_KHR_maintenance1`, `VK_KHR_maintenance2`, `VK_KHR_maintenance3`, `VK_KHR_maintenance4`, `VK_KHR_maintenance5`, `VK_KHR_maintenance6`, `VK_KHR_maintenance7`, `VK_KHR_maintenance8`, `VK_KHR_maintenance9`, `VK_KHR_maintenance10`, `VK_KHR_map_memory2`, `VK_KHR_multiview`, `VK_KHR_pipeline_binary`, `VK_KHR_pipeline_executable_properties`, `VK_KHR_pipeline_library`, `VK_KHR_present_id`, `VK_KHR_present_id2`, `VK_KHR_present_wait`, `VK_KHR_present_wait2`, `VK_KHR_push_descriptor`, `VK_KHR_relaxed_block_layout`, `VK_KHR_robustness2`, `VK_KHR_sampler_mirror_clamp_to_edge`, `VK_KHR_sampler_ycbcr_conversion`, `VK_KHR_separate_depth_stencil_layouts`, `VK_KHR_shader_atomic_int64`, `VK_KHR_shader_clock`, `VK_KHR_shader_draw_parameters`, `VK_KHR_shader_expect_assume`, `VK_KHR_shader_float_controls`, `VK_KHR_shader_float_controls2`, `VK_KHR_shader_float16_int8`, `VK_KHR_shader_fma`, `VK_KHR_shader_integer_dot_product`, `VK_KHR_shader_maximal_reconvergence`, `VK_KHR_shader_non_semantic_info`, `VK_KHR_shader_quad_control`, `VK_KHR_shader_relaxed_extended_instruction`, `VK_KHR_shader_subgroup_extended_types`, `VK_KHR_shader_subgroup_rotate`, `VK_KHR_shader_subgroup_uniform_control_flow`, `VK_KHR_shader_terminate_invocation`, `VK_KHR_shader_untyped_pointers`, `VK_KHR_spirv_1_4`, `VK_KHR_storage_buffer_storage_class`, `VK_KHR_swapchain`, `VK_KHR_swapchain_maintenance1`, `VK_KHR_swapchain_mutable_format`, `VK_KHR_synchronization2`, `VK_KHR_timeline_semaphore`, `VK_KHR_unified_image_layouts`, `VK_KHR_uniform_buffer_standard_layout`, `VK_KHR_variable_pointers`, `VK_KHR_vertex_attribute_divisor`, `VK_KHR_vulkan_memory_model`, `VK_KHR_workgroup_memory_explicit_layout`, `VK_KHR_zero_initialize_workgroup_memory`

**EXT (81)**: `VK_EXT_4444_formats`, `VK_EXT_attachment_feedback_loop_layout`, `VK_EXT_border_color_swizzle`, `VK_EXT_buffer_device_address`, `VK_EXT_calibrated_timestamps`, `VK_EXT_conditional_rendering`, `VK_EXT_conservative_rasterization`, `VK_EXT_color_write_enable`, `VK_EXT_custom_border_color`, `VK_EXT_debug_marker`, `VK_EXT_depth_bias_control`, `VK_EXT_depth_clamp_control`, `VK_EXT_depth_clamp_zero_one`, `VK_EXT_depth_clip_control`, `VK_EXT_depth_clip_enable`, `VK_EXT_descriptor_buffer`, `VK_EXT_descriptor_indexing`, `VK_EXT_device_generated_commands`, `VK_EXT_discard_rectangles`, `VK_EXT_image_drm_format_modifier`, `VK_EXT_dynamic_rendering_unused_attachments`, `VK_EXT_extended_dynamic_state`, `VK_EXT_extended_dynamic_state2`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_global_priority`, `VK_EXT_global_priority_query`, `VK_EXT_graphics_pipeline_library`, `VK_EXT_hdr_metadata`, `VK_EXT_host_query_reset`, `VK_EXT_image_2d_view_of_3d`, `VK_EXT_image_robustness`, `VK_EXT_image_sliced_view_of_3d`, `VK_EXT_image_view_min_lod`, `VK_EXT_index_type_uint8`, `VK_EXT_inline_uniform_block`, `VK_EXT_legacy_vertex_attributes`, `VK_EXT_line_rasterization`, `VK_EXT_load_store_op_none`, `VK_EXT_map_memory_placed`, `VK_EXT_memory_budget`, `VK_EXT_multi_draw`, `VK_EXT_mutable_descriptor_type`, `VK_EXT_nested_command_buffer`, `VK_EXT_non_seamless_cube_map`, `VK_EXT_pipeline_creation_cache_control`, `VK_EXT_pipeline_creation_feedback`, `VK_EXT_pipeline_robustness`, `VK_EXT_physical_device_drm`, `VK_EXT_post_depth_coverage`, `VK_EXT_present_timing`, `VK_EXT_primitive_topology_list_restart`, `VK_EXT_private_data`, `VK_EXT_primitives_generated_query`, `VK_EXT_provoking_vertex`, `VK_EXT_queue_family_foreign`, `VK_EXT_robustness2`, `VK_EXT_sample_locations`, `VK_EXT_sampler_filter_minmax`, `VK_EXT_scalar_block_layout`, `VK_EXT_separate_stencil_usage`, `VK_EXT_shader_atomic_float`, `VK_EXT_shader_image_atomic_int64`, `VK_EXT_shader_demote_to_helper_invocation`, `VK_EXT_shader_module_identifier`, `VK_EXT_shader_object`, `VK_EXT_shader_replicated_composites`, `VK_EXT_shader_subgroup_ballot`, `VK_EXT_shader_subgroup_vote`, `VK_EXT_shader_viewport_index_layer`, `VK_EXT_shader_uniform_buffer_unsized_array`, `VK_EXT_subgroup_size_control`, `VK_EXT_swapchain_maintenance1`, `VK_EXT_texel_buffer_alignment`, `VK_EXT_tooling_info`, `VK_EXT_transform_feedback`, `VK_EXT_vertex_attribute_divisor`, `VK_EXT_vertex_input_dynamic_state`, `VK_EXT_ycbcr_2plane_444_formats`, `VK_EXT_ycbcr_image_arrays`, `VK_EXT_zero_initialize_device_memory`

**AMD (1)**: `VK_AMD_buffer_marker`

**GOOGLE (3)**: `VK_GOOGLE_decorate_string`, `VK_GOOGLE_hlsl_functionality1`, `VK_GOOGLE_user_type`

**MESA (1)**: `VK_MESA_image_alignment_control`

**NV (1)**: `VK_NV_shader_sm_builtins`

**NVX (1)**: `VK_NVX_image_view_handle`

**VALVE (1)**: `VK_VALVE_mutable_descriptor_type`

</details>

### Unsupported Extensions

| Extension | Needs |
|---|---|
| `VK_KHR_compute_shader_derivatives`, `VK_NV_compute_shader_derivatives` | Turing |
| `VK_KHR_cooperative_matrix` | Turing |
| `VK_KHR_draw_indirect_count` | Turing |
| `VK_KHR_fragment_shader_barycentric` | Turing |
| `VK_KHR_fragment_shading_rate` | Turing |
| `VK_EXT_host_image_copy` | Turing (this is what holds the device at Vulkan 1.3) |
| `VK_EXT_mesh_shader` | Turing |
| `VK_NV_shader_atomic_float16_vector` | Turing |
| `VK_EXT_depth_range_unrestricted` | Volta |
| `VK_EXT_pci_bus_info` | DGPU |
| `VK_GOOGLE_display_timing` | a WSI platform that can promise it (in theory possible for GM20B) |

Sparse binding is off as well, although technically supported.

A few extensions are advertised by NVK's shared code but do nothing
since there is no DRM:
`VK_KHR_external_memory_fd`, `VK_KHR_external_fence_fd`,
`VK_KHR_external_semaphore_fd`, `VK_EXT_external_memory_dma_buf`,
`VK_EXT_physical_device_drm` and `VK_EXT_image_drm_format_modifier`.

`textureCompressionASTC_LDR` reads `VK_TRUE` here

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

### Linking against the installed portlib

Once you've run `sudo make install`, the driver is a functionally a normal devkitPro portlib and its `.pc` file
carries all of the flags above for you. Use devkitPro's target `pkg-config`:

```shell
 PKGCONF=$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config
 $PKGCONF --cflags nxvk    # -I…/include -D__SWITCH__ -DVK_USE_PLATFORM_VI_NN -ffunction-sections -fdata-sections
 $PKGCONF --libs   nxvk    # -L…/lib -Wl,--whole-archive -lnvk -Wl,--no-whole-archive
                           #   -Wl,--start-group -lnvk_support -lz -lexpat -Wl,--end-group
                           #   -Wl,-u,vk_icdGetInstanceProcAddr -Wl,--gc-sections
```

An example being:

```shell
 GCC=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc
 GXX=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++
 PKGCONF=$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config
 ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"

 $GCC -c main.c -o main.o $ARCH -I$DEVKITPRO/libnx/include $($PKGCONF --cflags nxvk)
 $GXX -specs=$DEVKITPRO/libnx/switch.specs $ARCH -o app.elf \
     main.o \
     $($PKGCONF --libs nxvk) \
     -lnx -lc -lm -lstdc++ -pthread
```

The system libs (`-lnx -lc -lm -lstdc++ -pthread`) must go after `$(pkg-config --libs nxvk)`.
For GL, use `nxvk-gl` in place of `nxvk` (it pulls in `libnvk` too). If your app touches the
newlib gaps directly, add `switch/smoke/nvk_compat.c` to your objects as noted above.

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
entrypoint-resolution macros (and debug messenging all wired up too). The `nvk_*.c` apps in `switch/smoke/`
are examples of how to do all of this, from a headless fill-and-readback up to a WSI swapchain present.

## OpenGL Usage

Same deal as Vulkan really. No loader, no GLVND, no shared libraries, etcetera. Zink, Gallium, the GL frontend and
EGL all link statically into your `.nro`, and Zink finds NVK through the very same `vk_icdGetInstanceProcAddr`
symbol.

Currently, you get OpenGL 4.5 core, a compatibility context, and OpenGL ES 2.0 / 3.2.

Note that performance will differ from switch-mesa's nouveau driver. Whether performance is greater or not
is entirely dependent on the scene, and can only be tested via trying it yourself. NXVK over switch-mesa also
adds ~15MBs to the binary for including both Vulkan and GL.

It is recommended not to link in GL support if your project doesn't use GL.

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

Everything from the Vulkan linking rules still apply, and in addition:

- The GL archives go inside `--whole-archive` alongside `libnvk.a`.
- Do **not** link `libglsl_util.a`. It multiply-defines `_mesa_error_no_memory` against `libmesa.a`.
- Compile your GL sources with `-DHAVE_PTHREAD` and Mesa's internal include set (see `GL_INC` in `build-nro.sh`)

If you installed with `sudo make install-gl`, all of this is already baked into the `nxvk-gl` portlib
(See [Linking against the installed portlib](#linking-against-the-installed-portlib)).

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

    /* Do your draws, then: */
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
