# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Toolchain image that cross-builds Mesa/NVK (Vulkan) for the Nintendo Switch (GM20B).
#
# Build (ested on Fedora Linux with Podman):
#   podman build -t nvk-switch-build switch/docker
#
# Everything runs inside the image with the repo bind-mounted at /work:
#   podman run --rm -v "$PWD:/work:z" -w /work nvk-switch-build bash switch/docker/verify-toolchain.sh
FROM docker.io/devkitpro/devkita64

ENV DEBIAN_FRONTEND=noninteractive

# Build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3-pip bison flex curl ca-certificates pkg-config git \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --break-system-packages --no-cache-dir 'meson>=1.4' mako pyyaml

# Rust
ENV RUSTUP_HOME=/opt/rust/rustup CARGO_HOME=/opt/rust/cargo
ENV PATH="/opt/rust/cargo/bin:${PATH}"
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --default-toolchain nightly --profile minimal --component rust-src \
    && rustc --version && cargo --version

# bindgen: C->Rust FFI for NAK. cbindgen: Rust->C headers for NIL.
RUN cargo install --locked bindgen-cli cbindgen && bindgen --version && cbindgen --version

# rustfmt (I think only needed to prevent meson errors and not actually for compilation)
RUN rustup component add rustfmt && rustfmt --version

# CLC / SPIR-V stack for mesa_clc
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake \
        llvm-15-dev clang-15 libclang-15-dev \
        libclc-15-dev libclc-15 \
        llvm-spirv-15 libllvmspirvlib-15-dev \
        libdrm-dev \
        glslang-tools \
    && rm -rf /var/lib/apt/lists/* \
    && glslangValidator --version
ENV LLVM_CONFIG=/usr/bin/llvm-config-15
ENV LIBCLANG_PATH=/usr/lib/llvm-15/lib

RUN echo "deb http://deb.debian.org/debian trixie main" > /etc/apt/sources.list.d/trixie.list \
    && printf 'Package: *\nPin: release n=trixie\nPin-Priority: 100\n' > /etc/apt/preferences.d/trixie-pin \
    && apt-get update \
    && apt-get install -y --no-install-recommends -t trixie spirv-tools \
    && rm -rf /var/lib/apt/lists/*

# Cross-build plumbing
RUN mkdir -p /opt/switch-cross-include/libdrm \
    && cp /usr/include/xf86drm.h /usr/include/xf86drmMode.h /opt/switch-cross-include/ 2>/dev/null || true \
    && cp -r /usr/include/libdrm/* /opt/switch-cross-include/ 2>/dev/null || true \
    && cp -r /usr/include/libdrm/* /opt/switch-cross-include/libdrm/ 2>/dev/null || true \
    && ls /opt/switch-cross-include/ | head

# Cross target stubs resolved by GCC and bindgen from -I/opt/switch-cross-include.
COPY cross-include/ /opt/switch-cross-include/

# Bake the host CLC/SPIR-V .pc files into the Switch CROSS pkg-config dir.
RUN XDIR=/opt/devkitpro/portlibs/switch/lib/pkgconfig; mkdir -p "$XDIR"; \
    for name in libclc LLVMSPIRVLib SPIRV-Tools SPIRV-Tools-shared SPIRV-Headers; do \
      f=$(find /usr -name "$name.pc" 2>/dev/null | head -1); \
      if [ -n "$f" ]; then cp "$f" "$XDIR/"; echo "baked $f"; fi; \
    done

# Provide an empty libdl.a for static dlopen fallbacks.
RUN /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs \
        /opt/devkitpro/portlibs/switch/lib/libdl.a

WORKDIR /work
