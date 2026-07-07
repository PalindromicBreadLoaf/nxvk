# NXVK Validation Programs

These nine `nvk_*.c` apps are used as testing to verify that nothing breaks upon
any update of the included Mesa version.
Every app is a standalone `.nro` that runs completely headless (besides 5.9 which does present)
and logs each stage to a log of the same name on the root of the SD card.

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

## Build

```bash
# Only needed once
podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build bash -lc '
  export PATH=/work/switch/build/native-tools/bin:$PATH
  bash switch/build/build-native-tools.sh
  bash switch/build/configure-mesa.sh
  ninja -k0 -C switch/build/cross src/nouveau/vulkan/libvulkan_nouveau.so || true'
# (the final .so link fails intentionally. Don't worry about the error)

# regenerate shaders
podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build bash switch/smoke/shaders/gen-shaders.sh

# build one .nro (or loop over all nine)
podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build bash switch/build/build-nro.sh nvk_smoke
```
