# NXVK
(NVK + NX = NXVK? NXNVK? NVKNX?)

A subset of the **Mesa** project that ports the [NVK](https://docs.mesa3d.org/drivers/nvk.html) Vulkan driver to the Nintendo Switch.

Currently, this is based on **Mesa 25.0.7**.

## Licensing

Upstream Mesa code retains its existing licences. All new files added by this fork are MIT-licensed
This fork does **not** copy any GPL-licensed source from the `switch-nvk` project.
Hardware behaviour was learned from its documentation and then reimplemented by hand.

## Layout

The `switch/` directory contains all Switch-specific build scripts, toolchain, docs, and whatever else is needed. 
This is to both allow for easier rebasing on newer Mesa versions, and to provide an easy place to manage all changes for this port.

### Exceptions

```
src/util/detect_os.h
src/c11/impl/threads_posix.c
src/nouveau/vulkan/nvk_descriptor_table.c
src/nouveau/vulkan/nvk_device_memory.c
src/nouveau/vulkan/nvk_instance.c
src/nouveau/vulkan/nvk_physical_device.c
src/nouveau/vulkan/nvk_physical_device.h
src/util/os_misc.c
src/util/u_debug.c
src/util/u_debug.h
src/util/u_endian.h
src/vulkan/runtime/vk_image.c
src/vulkan/runtime/vk_image.h
src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_pdev.c
src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_dev.c
src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_va.c
src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_mem.c
src/nouveau/vulkan/nvkmd/nvgpu/nvkmd_nvgpu_ctx.c
 ```

## Contributing

All contributions are welcome, however, prior to doing any work I recommend opening an issue to discuss what you wish to do
so that it aligns with the project's scope.
