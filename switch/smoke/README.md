# NXVK Validation Programs

The `nvk_*` apps validate the Vulkan driver, the `gl_*`/`gles*` apps
validate OpenGL through Zink on top of it. Every app is a standalone 
`.nro` and logs each stage to a log of the same name on the root of
the SD card.

## Vulkan

These nine `nvk_*.c` apps are used as testing to verify that nothing breaks upon
any update of the included Mesa version.
Every app runs completely headless (besides 5.9 which does present).

| App                | Stage | Test                                                 |
|--------------------|-------|------------------------------------------------------|
| `nvk_smoke`        | 1     | memory + VA bind + submit + coherency                |
| `nvk_tri`          | 2     | NAK shaders execute                                  |
| `nvk_logo`         | 3     | `sampler2D`, descriptor sets, buffer to image upload |
| `nvk_scene`        | 4     | 3D cube + ZETA depth                                 |
| `nvk_indexed`      | 5     | `vkCmdDrawIndexed`, UINT16 + UINT32                  |
| `nvk_multi`        | 6     | many draws, pipeline switches, alpha blending        |
| `nvk_textures`     | 7     | mipmaps + sRGB decode + BC1 decompression            |
| `nvk_cubemap`      | 8     | `CUBE_COMPATIBLE` image + `samplerCube`              |
| `nvk_vi_swapchain` | 9     | WSI present path                                     |

## OpenGL (via Zink)

`gl_linktest` and `gl_gallium` are scaffolding rather than proper feature tests.
The first only proves the link worked, and the second drives Gallium directly with no
GL frontend. `gl_smoke` and `gl_tri` render headless through `gl_harness.h`.
Everything from `gl_tex` on comes up through EGL on a window surface
(`gl_egl_harness.h`), verifies fixed probe pixels with `glReadPixels` before the
swap, then presents for ~3 s — so each one is both self-checking and visible.
Mesa/Zink diagnostics go to `sdmc:/<app>_mesa.log` alongside the app's own log.

| App           | Stage | Test                                                          |
|---------------|-------|---------------------------------------------------------------|
| `gl_linktest` | —     | static link only                                              |
| `gl_gallium`  | —     | raw Gallium `clear_buffer` + readback                         |
| `gl_caps`     | 0     | emergent GL/GLES version, extension list, gate limits         |
| `gl_smoke`    | 1     | `glClear` + `glReadPixels`, headless                          |
| `gl_tri`      | 2     | `glDrawArrays` triangle, GLSL through NAK, headless           |
| `gl_tex`      | 3     | `glTexImage2D` upload, sampler state, UV interpolation        |
| `gl_fbo`      | 4     | render to an FBO with depth, then sample the result           |
| `gl_ubo_vbo`  | 5     | VBO/IBO `glDrawElements` + two std140 uniform blocks          |
| `gl_egl_tri`  | 6     | EGL window surface + `eglSwapBuffers` present                 |
| `gl_multi`    | 7     | 64 draws, two programs, state churn, blending, scissor        |
| `gles3`       | 8     | GLES3 instancing, MRT, transform feedback                     |
| `gl_feat`     | 9     | desktop GL core instancing, geometry shader, tessellation     |
| `gl_diag`     | —     | diagnostic to isolate which readback axis is broken           |

## Build

```bash
# Only needed once
podman run --rm -v "$PWD:/work:z" -w /work nxvk bash -lc '
  export PATH=/work/switch/build/native-tools/bin:$PATH
  bash switch/build/build-native-tools.sh
  bash switch/build/configure-mesa.sh
  ninja -k0 -C switch/build/cross src/nouveau/vulkan/libvulkan_nouveau.so || true'
# (the final .so link fails intentionally. Don't worry about the error)

# regenerate shaders
podman run --rm -v "$PWD:/work:z" -w /work nxvk bash switch/smoke/shaders/gen-shaders.sh

# build one .nro (or loop over all nine)
podman run --rm -v "$PWD:/work:z" -w /work nxvk bash switch/build/build-nro.sh nvk_smoke
```

The GL apps need the Zink build dir instead. The same toolchain image is used, just a different
configure and a `CROSS_BUILD` pointing at it:

```bash
# Only needed once
podman run --rm -v "$PWD:/work:z" -w /work nxvk bash -lc '
  export PATH=/work/switch/build/native-tools/bin:$PATH
  bash switch/build/configure-zink.sh
  ninja -k0 -C switch/build/cross-zink src/nouveau/vulkan/libvulkan_nouveau.so || true'

podman run --rm -v "$PWD:/work:z" -w /work \
  -e CROSS_BUILD=/work/switch/build/cross-zink \
  nxvk bash switch/build/build-nro.sh gl_tri
```
