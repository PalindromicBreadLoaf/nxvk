# Standalone Switch application example

From this directory, in a devkitPro environment:

```sh
bash ./build.sh
```

This discovers the package root two directories above and produces
`out/nxvk_app_example.nro`. Optional arguments select another package root and
output directory. The executable performs a headless Vulkan buffer fill/readback,
logs to `sdmc:/nxvk_app_example.log`, then waits for `+` to exit. It never returns
to the NXVK test runner or modifies its state. Use full-memory/title-override mode.

**Review `example_compat.c` first.** The wrapper explicitly opts into its limited
POSIX/newlib fallbacks for this demonstration. They are copied from the tested
smoke scaffolding, not supplied as a production runtime. Regex never matches,
flock does not lock, and sysconf memory reporting is simplified. Existing apps
should supply their own compatible implementations, not compile this file on top
of them. Direct CMake configuration requires the explicit
`NXVK_EXAMPLE_ALLOW_LIMITED_COMPAT=ON` acknowledgement.

The example owns its `__nx_applet_type` and `__nx_heap_size`. Do not copy those
globals into an application that already defines its own startup configuration.
No swapchain/window is created; console text is provided by libnx. For an existing
renderer, use the static resolver and VI surface integration described in
`INTEGRATION.md` at the package root.
