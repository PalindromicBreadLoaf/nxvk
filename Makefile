# Copyright © 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build and install for NXVK.
#
#   make                    build the Vulkan driver and stage the portlib package
#   make gl                 also build the OpenGL (Zink) stack and its package
#   sudo make install       copy the package into $(DEVKITPRO)/portlibs/switch
#   sudo make install-gl    same, including the OpenGL archive
#   make nro APP=nvk_smoke  link a smoke app into a runnable .nro
#   make clean / make distclean

SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
.ONESHELL:
.DEFAULT_GOAL := all

CONTAINER   ?= podman
IMAGE       ?= nxvk
DEVKITPRO   ?= /opt/devkitpro
PKG_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)

DKA64  := $(DEVKITPRO)/devkitA64/bin
AR     := $(DKA64)/aarch64-none-elf-ar
PORTLIB := $(DEVKITPRO)/portlibs/switch

CROSS  := switch/build/cross
ZINK   := switch/build/cross-zink
PKGDIR := switch/build/pkg

NATIVE_STAMP := switch/build/native-tools/bin/mesa_clc
CROSS_STAMP  := $(CROSS)/build.ninja
ZINK_STAMP   := $(ZINK)/build.ninja

ifeq ($(strip $(CONTAINER)),)
DRUN :=
else
DRUN := $(CONTAINER) run --rm -v "$(CURDIR):/work:z" -w /work $(IMAGE)
endif

# Driver support archives
SUPPORT_LIBS := \
  src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a \
  src/c11/impl/libmesa_util_c11.a \
  src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a \
  src/compiler/rust/libcompiler_c_helpers.a \
  src/nouveau/headers/libnvidia_headers_c.a \
  src/nouveau/nil/libnil.a src/nouveau/nil/liblibnil_format_table.a \
  src/compiler/nir/libnir.a src/compiler/libcompiler.a \
  src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a \
  src/vulkan/util/libvulkan_util.a src/compiler/spirv/libvtn.a \
  src/util/libxmlconfig.a

# OpenGL frontend archives
GL_LIBS := \
  src/egl/libEGL.a \
  src/mesa/libmesa.a \
  src/compiler/glsl/libglsl.a src/compiler/glsl/glcpp/libglcpp.a \
  src/mesa/glapi/shared-glapi/libglapi.a src/mesa/glapi/glapi/libglapi_bridge.a \
  src/gallium/auxiliary/libgallium.a src/gallium/drivers/zink/libzink.a \
  src/gallium/winsys/zink/drm/libzinkwinsys.a \
  src/gallium/winsys/sw/null/libws_null.a src/gallium/winsys/sw/wrapper/libwsw.a

DRIVER_TARGETS := src/nouveau/vulkan/libnvk.a $(SUPPORT_LIBS)
GL_TARGETS     := $(GL_LIBS)

NINJA_ENV := cp -r switch/docker/cross-include/. /opt/switch-cross-include/ 2>/dev/null || true; \
             export PATH="$$(pwd)/switch/build/native-tools/bin:$$PATH";

.PHONY: all gl image ensure-image driver zink package package-gl \
        install install-gl uninstall nro clean distclean help

all: package

gl: package-gl

## toolchain image

image:
	$(CONTAINER) build -t $(IMAGE) switch/docker

ensure-image:
	@if [ -n "$(CONTAINER)" ] && ! $(CONTAINER) image inspect $(IMAGE) >/dev/null 2>&1; then
	  echo ">> toolchain image $(IMAGE) missing; building it"
	  $(CONTAINER) build -t $(IMAGE) switch/docker
	fi

## build

$(NATIVE_STAMP): | ensure-image
	@echo ">> building native tools"
	$(DRUN) bash switch/build/build-native-tools.sh

$(CROSS_STAMP): | $(NATIVE_STAMP)
	@echo ">> configuring Vulkan build"
	$(DRUN) bash switch/build/configure-mesa.sh

driver: $(CROSS_STAMP)
	@echo ">> building driver archives"
	$(DRUN) bash -lc '$(NINJA_ENV) ninja -C $(CROSS) $(DRIVER_TARGETS)'
	@test -f $(CROSS)/src/nouveau/vulkan/libnvk.a || { echo "ERROR: libnvk.a not produced"; exit 1; }

$(ZINK_STAMP): | $(NATIVE_STAMP)
	@echo ">> configuring OpenGL build"
	$(DRUN) bash switch/build/configure-zink.sh

zink: $(ZINK_STAMP)
	@echo ">> building Zink/Gallium/GL archives"
	$(DRUN) bash -lc '$(NINJA_ENV) ninja -C $(ZINK) $(GL_TARGETS)'
	@test -f $(ZINK)/src/gallium/drivers/zink/libzink.a || { echo "ERROR: libzink.a not produced"; exit 1; }

## packaging

# Bundle a set of archives from $1 into a single archive $2 via an ar MRI script.
define bundle
	printf 'create %s\n' "$(2)" > $(2).mri
	for a in $(3); do printf 'addlib %s\n' "$(1)/$$a" >> $(2).mri; done
	printf 'save\nend\n' >> $(2).mri
	$(AR) -M < $(2).mri
	rm -f $(2).mri
endef

package: driver
	@echo ">> staging Vulkan portlib package in $(PKGDIR)"
	mkdir -p $(PKGDIR)/lib/pkgconfig
	$(call bundle,$(CROSS),$(PKGDIR)/lib/libnvk.a,src/nouveau/vulkan/libnvk.a)
	$(call bundle,$(CROSS),$(PKGDIR)/lib/libnvk_support.a,$(SUPPORT_LIBS))
	printf '%s\n' \
	  'prefix=$(PORTLIB)' \
	  'exec_prefix=$${prefix}' \
	  'libdir=$${prefix}/lib' \
	  'includedir=$${prefix}/include' \
	  '' \
	  'Name: nxvk' \
	  'Description: NVK ported to the Nintendo Switch' \
	  'Version: $(PKG_VERSION)' \
	  'Libs: -L$${libdir} -Wl,--whole-archive -lnvk -Wl,--no-whole-archive -Wl,--start-group -lnvk_support -lz -lexpat -Wl,--end-group -Wl,-u,vk_icdGetInstanceProcAddr -Wl,--gc-sections' \
	  'Cflags: -I$${includedir} -D__SWITCH__ -DVK_USE_PLATFORM_VI_NN -ffunction-sections -fdata-sections' \
	  > $(PKGDIR)/lib/pkgconfig/nxvk.pc
	@echo ">> staged: libnvk.a libnvk_support.a nxvk.pc"

package-gl: zink package
	@echo ">> staging OpenGL portlib package in $(PKGDIR)"
	$(call bundle,$(ZINK),$(PKGDIR)/lib/libnvk_gl.a,$(GL_LIBS))
	printf '%s\n' \
	  'prefix=$(PORTLIB)' \
	  'exec_prefix=$${prefix}' \
	  'libdir=$${prefix}/lib' \
	  'includedir=$${prefix}/include' \
	  '' \
	  'Name: nxvk-gl' \
	  'Description: NVK + Zink ported to the Nintendo Switch' \
	  'Version: $(PKG_VERSION)' \
	  'Libs: -L$${libdir} -Wl,--whole-archive -lnvk_gl -lnvk -Wl,--no-whole-archive -Wl,--start-group -lnvk_support -lz -lexpat -Wl,--end-group -Wl,-u,vk_icdGetInstanceProcAddr -Wl,--gc-sections' \
	  'Cflags: -I$${includedir} -D__SWITCH__ -DVK_USE_PLATFORM_VI_NN -DHAVE_PTHREAD -ffunction-sections -fdata-sections' \
	  > $(PKGDIR)/lib/pkgconfig/nxvk-gl.pc
	@echo ">> staged: libnvk_gl.a nxvk-gl.pc"

## install

install:
	@test -f $(PKGDIR)/lib/libnvk.a || { echo "ERROR: run 'make' first"; exit 1; }
	@echo ">> installing into $(PORTLIB)"
	install -d $(PORTLIB)/lib/pkgconfig $(PORTLIB)/include $(PORTLIB)/licenses/nxvk
	install -m644 $(PKGDIR)/lib/libnvk.a         $(PORTLIB)/lib/
	install -m644 $(PKGDIR)/lib/libnvk_support.a $(PORTLIB)/lib/
	install -m644 $(PKGDIR)/lib/pkgconfig/nxvk.pc $(PORTLIB)/lib/pkgconfig/
	cp -r include/vulkan include/vk_video $(PORTLIB)/include/
	install -m644 licenses/GPL-2.0-or-later $(PORTLIB)/licenses/nxvk/
	@echo ">> installed nxvk $(PKG_VERSION); use 'pkg-config --libs nxvk' to link"

install-gl: install
	@test -f $(PKGDIR)/lib/libnvk_gl.a || { echo "ERROR: run 'make gl' first"; exit 1; }
	install -m644 $(PKGDIR)/lib/libnvk_gl.a        $(PORTLIB)/lib/
	install -m644 $(PKGDIR)/lib/pkgconfig/nxvk-gl.pc $(PORTLIB)/lib/pkgconfig/
	@echo ">> installed nxvk-gl $(PKG_VERSION)"

uninstall:
	@echo ">> removing nxvk from $(PORTLIB)"
	rm -f $(PORTLIB)/lib/libnvk.a $(PORTLIB)/lib/libnvk_support.a $(PORTLIB)/lib/libnvk_gl.a
	rm -f $(PORTLIB)/lib/pkgconfig/nxvk.pc $(PORTLIB)/lib/pkgconfig/nxvk-gl.pc
	rm -rf $(PORTLIB)/licenses/nxvk

## validation

APP ?= nvk_smoke
nro: driver
	$(DRUN) bash switch/build/build-nro.sh $(APP)

clean:
	rm -rf $(CROSS) $(ZINK) $(PKGDIR) switch/smoke/out

distclean: clean
	rm -rf switch/build/native switch/build/native-tools

help:
	@echo "Targets:"
	@echo "  all (default)  build Vulkan driver and stage the portlib package"
	@echo "  gl             also build the OpenGL stack and stage its package"
	@echo "  image          build the podman/docker toolchain image"
	@echo "  install        install the package into \$$(DEVKITPRO)/portlibs/switch (needs root)"
	@echo "  install-gl     install including the OpenGL archive + .pc (needs root)"
	@echo "  uninstall      remove the installed nxvk files (needs root)"
	@echo "  nro APP=<name> link a smoke app into switch/smoke/out/<name>.nro"
	@echo "  clean          remove cross build dirs and the staged package"
	@echo "  distclean      also remove native tools"
	@echo ""
	@echo "Vars: CONTAINER=$(CONTAINER)  IMAGE=$(IMAGE)  DEVKITPRO=$(DEVKITPRO)"
	@echo "  (set CONTAINER=  to run build steps directly)"
