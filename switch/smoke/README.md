# NXVK Validation Programs

The `nvk_*` apps validate the Vulkan driver, the `gl_*`/`gles*` apps
validate OpenGL through Zink on top of it. Every app is a standalone 
`.nro` and logs each stage to a log of the same name on the root of
the SD card.

Each app ends its log with a `=== <app> PASSED ===` or `=== <app> FAILED ===`
line, which is what [the runner](#running-the-whole-ladder) looks for.

Not every file here is an app. `nvk_harness.h` (bring-up), `nvk_gfx.h`
(pipeline scaffolding), `nvk_math.h`, `nvk_cube.h` and `gl_harness.h` 
/ `gl_egl_harness.h` are headers. `nvk_compat.c` supplies the
newlib gaps, and `nvk_chain.c` is the runner shim.

## Vulkan

### Milestone ladder

These nine must pass before submitting a PR (ideally all are ran, however).
Every one runs headless except for 9 which presents.

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

### Behaviour and regression probes

Everything past the ladder. These exist because each one caught, or is there to
catch, a specific hardware or driver behaviour that a render test cannot see.
Several are A/B tests: they run the same work twice with an `NVK_DEBUG` flag
flipped and fail if the two phases disagree. They are listed in the order the
runner walks them.

| App               | Test                                                                                       |
|-------------------|--------------------------------------------------------------------------------------------|
| `nvk_present`     | `IMMEDIATE` really does present before vsync                                               |
| `nvk_compress`    | compressed vs `no_compression` colour target render and round-trip identically             |
| `nvk_sector`      | texture header sector promotion vs `no_sector_promotion`                                   |
| `nvk_zcull`       | coarse depth culling                                                                       |
| `nvk_push_desc`   | push descriptor sets land in the right slot                                                |
| `nvk_desc_flush`  | submit cost stays flat                                                                     |
| `nvk_cmd_flush`   | command buffer flush cost                                                                  |
| `nvk_ce_copy`     | copy-engine multi-line copies                                                              |
| `nvk_engine_wait` | a cross-submit semaphore wait is honoured by the engine                                    |
| `nvk_b2_flush`    | fence signalling verification                                                              |
| `nvk_mem_churn`   | 48 rounds of alloc/free recycle backing stores                                             |
| `nvk_subtile`     | fragment/compute rates per knob, readbacks must match                                      |

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

## Running the whole ladder

`nvk_runner` runs all 34 apps unattended. It chainloads each `.nro` in turn.
Any app that faults partway is recorded, and the test is continued. Leaving
mid-run causes `nvk_runner` to remember where it left and it will continue 
from there on the next run.

Copy the runner and every `.nro` you want run into the same directory on the SD
card (i.e. `sdmc:/switch/nxvk-tests`) and launch `nvk_runner` from a loader that
supports chainloading (`envHasNextLoad`).

Output lands in `sdmc:/nxvk_run/`:

| File              | What it holds                                                |
|-------------------|--------------------------------------------------------------|
| `summary.log`     | one `PASS`/`FAIL`/`GONE`/`SKIP` line per app                 |
| `<app>.log`       | that app's log                                               |
| `<app>.mesa.log`  | the driver log for that app                                  |
| `chain.log`       | chainload trace                                              |
| `state`           | resume cursor                                                |

`GONE` means the log has no verdict line at all, meaning the app hung or faulted.
The final screen prints the tally and waits for `+` to exit.

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

# build one .nro
podman run --rm -v "$PWD:/work:z" -w /work nxvk bash switch/build/build-nro.sh nvk_smoke
```

`make nro APP=nvk_smoke` from the repo root does the same thing.

`nvk_runner` is the one app that links no driver at all, so it can build without a
cross build or driver build present:

```bash
podman run --rm -v "$PWD:/work:z" -w /work nxvk bash switch/build/build-nro.sh nvk_runner
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
