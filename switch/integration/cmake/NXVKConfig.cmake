# SPDX-License-Identifier: GPL-2.0-or-later
# Relocatable static Vulkan ICD. The consuming project supplies its Horizon
# toolchain/startup specs and application-specific POSIX compatibility.
get_filename_component(_NXVK_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
foreach(_file IN ITEMS lib/libnvk.a lib/libnvk_support.a
                       include/vulkan/vulkan.h include/nxvk/nxvk_static.h)
    if(NOT EXISTS "${_NXVK_PREFIX}/${_file}")
        set(NXVK_FOUND FALSE)
        set(NXVK_NOT_FOUND_MESSAGE "Incomplete NXVK distribution: missing ${_file}")
        return()
    endif()
endforeach()

if(DEFINED DEVKITPRO AND NOT DEVKITPRO STREQUAL "")
    set(_NXVK_DKP "${DEVKITPRO}")
else()
    set(_NXVK_DKP "$ENV{DEVKITPRO}")
endif()
if(NOT EXISTS "${_NXVK_DKP}/libnx/include/switch.h")
    set(NXVK_FOUND FALSE)
    set(NXVK_NOT_FOUND_MESSAGE "Set DEVKITPRO to the Switch SDK (libnx/portlibs required)")
    return()
endif()

if(NOT TARGET NXVK::Driver)
    add_library(NXVK::Driver INTERFACE IMPORTED)
    set_target_properties(NXVK::Driver PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_NXVK_PREFIX}/include;${_NXVK_DKP}/libnx/include"
        INTERFACE_COMPILE_DEFINITIONS "__SWITCH__;VK_USE_PLATFORM_VI_NN"
        INTERFACE_COMPILE_OPTIONS "-ffunction-sections;-fdata-sections"
        INTERFACE_LINK_DIRECTORIES "${_NXVK_DKP}/portlibs/switch/lib;${_NXVK_DKP}/libnx/lib"
        INTERFACE_LINK_OPTIONS "-Wl,-u,vk_icdGetInstanceProcAddr;-Wl,--gc-sections"
        INTERFACE_LINK_LIBRARIES "-Wl,--whole-archive;${_NXVK_PREFIX}/lib/libnvk.a;-Wl,--no-whole-archive;-Wl,--start-group;${_NXVK_PREFIX}/lib/libnvk_support.a;z;expat;nx;c;m;stdc++;-pthread;-Wl,--end-group"
    )
endif()
set(NXVK_INCLUDE_DIR "${_NXVK_PREFIX}/include")
set(NXVK_FOUND TRUE)
unset(_NXVK_PREFIX)
unset(_NXVK_DKP)
unset(_file)
