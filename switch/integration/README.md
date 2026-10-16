# NXVK static application integration

This distribution contains the optimized **Vulkan-only** driver for Nintendo
Switch / Horizon / Cortex-A57 / GM20B. It is linked into your application, not
installed as an SD-card system driver. Rebuild the application after changing
the archives. Do not mix archives or headers from different NXVK/Mesa builds.

## Package layout

```text
include/vulkan/                 Vulkan headers
include/vk_video/               companion Vulkan headers
include/nxvk/nxvk_static.h      static ICD declaration (C and C++)
lib/libnvk.a                    driver
lib/libnvk_support.a            merged compiler/runtime support
lib/pkgconfig/nxvk.pc           relocatable pkg-config description
lib/cmake/NXVK/NXVKConfig.cmake CMake target NXVK::Driver
examples/switch-minimal/        self-contained headless client and build scripts
BUILDINFO.txt                  driver source/toolchain metadata
licenses/nxvk/                 port license
```

No Mesa source checkout, Rust compiler or SPIR-V generator is needed to build the
example from the package. A compatible **devkitPro/libnx/portlibs toolchain**, Bash,
CMake 3.16+ and a build tool such as Make or Ninja are still required. The required
SDK libraries include zlib, expat, libnx and C/C++/thread runtimes.

## Build the supplied example

From the extracted package root, in the devkitPro environment:

```sh
bash examples/switch-minimal/build.sh
```

Output: `examples/switch-minimal/out/nxvk_app_example.nro` (plus ELF and link map).
Run it on Switch in full-memory/title-override mode; it checks 1,024 filled words,
logs to `sdmc:/nxvk_app_example.log` and waits for `+`. This is a headless API/link
integration check, not a renderer or a game benchmark. CI builds and inspects it;
CI does not execute Horizon NROs.

The example explicitly opts into **limited demonstration compatibility code**.
`example_compat.c` is not a general POSIX implementation: regex always reports no
match, flock is a no-op, and sysconf memory reporting is simplified. The driver
CMake target does **not** add these shims. Integrating applications must reconcile
missing symbols with their own Horizon runtime; never hide conflicts with
`--allow-multiple-definition` or copy test-chain constructors into the app.

## Add the driver to an existing CMake application

Keep your application's working Horizon/devkitPro toolchain and startup specs.
Point `NXVK_DIR` at this distribution's `lib/cmake/NXVK` directory and set
`DEVKITPRO` (environment or CMake variable). Then, after creating your app target:

```cmake
find_package(NXVK CONFIG REQUIRED)
# Prefer the bundled headers if another SDK dependency also provides Vulkan.
target_include_directories(my_app BEFORE PRIVATE "${NXVK_INCLUDE_DIR}")
target_link_libraries(my_app PRIVATE NXVK::Driver)
```

Replace `my_app` with the real target. Use a C++ linker for the static support
libraries. `NXVK::Driver` supplies the archive grouping, ICD retention, SDK library
search paths and required libraries. It does not add a main(), change clocks,
select your applet type, add compatibility functions or override your toolchain's
linker script/specs. The standalone example has its own small toolchain file;
do not include that file on top of an already configured emulator toolchain.

## pkg-config alternative

New integration-capable packages use `pcfiledir` for a relocatable prefix. For
example, with `NXVK_ROOT` pointing to the extracted package:

```sh
PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR= \
  PKG_CONFIG_LIBDIR="$NXVK_ROOT/lib/pkgconfig" pkg-config --cflags --libs nxvk
```

Older build #7 packages have a fixed portlibs prefix: use
`--define-variable=prefix="$NXVK_ROOT"` with them. This override also remains
valid for new packages. The .pc file supplies the NVK/NAK dependencies; retain the
normal libnx/C/C++/thread runtime and Switch startup flags of your app. Do not
apply a second `--whole-archive` copy in addition to the CMake target.

## Static loader

Define `VK_NO_PROTOTYPES` before your first Vulkan include when doing manual
loading or using volk. Include `nxvk/nxvk_static.h`. Before instance creation:

```c
if (setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1) != 0) {
    /* Report initialization failure. */
}
PFN_vkGetInstanceProcAddr get_proc = vk_icdGetInstanceProcAddr;
```

The environment opt-in is required for the non-conformant GM20B target. Resolve
`vkCreateInstance` using `get_proc(VK_NULL_HANDLE, "vkCreateInstance")`, then load
instance/device procedures through the corresponding dispatch functions. Check
results and null procedure pointers. Do not try to dlopen a desktop libvulkan.
The sample `main.c` shows initialization, GPU work, readback and cleanup.

For an existing volk-based renderer, use
`volkInitializeCustom(vk_icdGetInstanceProcAddr)`, then load the instance and device
(or per-device tables). Volk is not bundled and a second copy is not needed.

For presentation, request instance extensions `VK_KHR_surface` and
`VK_NN_vi_surface`, device extension `VK_KHR_swapchain`, and a valid libnx `NWindow`
for `vkCreateViSurfaceNN`. Check queue-family presentation support and surface
capabilities. This is not a drop-in Win32/Xlib/Wayland surface or SDL loader.

## Verified integration build

[CI run #10](https://github.com/berlint-hub/nxvk/actions/runs/38943461562), source
`623323bebc559a61275436a01cdbd5e43095abd5`, successfully built and linked the
standalone application from a relocated package path containing a space.
The [integration artifact](https://github.com/berlint-hub/nxvk/actions/runs/38943461562/artifacts/12064098920)
contains that package and its prebuilt example NRO. This is build/link validation,
not an execution result for the new example on Switch.

## Validation boundaries and license

The package check builds the example from a **relocated extracted directory**,
checks its AArch64 ELF, verifies both archives' paths in the link map and rejects
test-runner state paths. It does not prove that a particular emulator was ported,
that all application APIs work, or that this newly built NRO was run on hardware.
The original optimized driver build #7 separately passed the 22-app hardware suite.

The port includes GPL-2.0-or-later code. Review the supplied license and your
application's distribution obligations; do not assume the whole fork is MIT-only.
