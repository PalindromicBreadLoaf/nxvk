# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(nxvk_app_example LANGUAGES C CXX)

option(NXVK_EXAMPLE_ALLOW_LIMITED_COMPAT
       "Use the sample-only newlib gap fillers; NOT a general app runtime" OFF)
if(NOT NXVK_EXAMPLE_ALLOW_LIMITED_COMPAT)
    message(FATAL_ERROR
        "Review example_compat.c and README.md; this example requires explicit "
        "-DNXVK_EXAMPLE_ALLOW_LIMITED_COMPAT=ON. NXVK::Driver itself adds no shims.")
endif()

find_package(NXVK CONFIG REQUIRED)
add_executable(nxvk_app_example main.c example_compat.c)
set_target_properties(nxvk_app_example PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES LINKER_LANGUAGE CXX SUFFIX ".elf")
target_compile_options(nxvk_app_example PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nxvk_app_example PRIVATE NXVK::Driver)
target_link_options(nxvk_app_example PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.map")

set(_tools "${DEVKITPRO}/tools/bin")
set(_strip "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-strip")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.nro"
    COMMAND "${_strip}" "$<TARGET_FILE:nxvk_app_example>"
        -o "${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.stripped.elf"
    COMMAND "${_tools}/nacptool" --create "NXVK app integration" "nxvk" "1.0.0"
        "${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.nacp"
    COMMAND "${_tools}/elf2nro" "${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.stripped.elf"
        "${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.nro"
        "--icon=${DEVKITPRO}/libnx/default_icon.jpg"
        "--nacp=${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.nacp"
    DEPENDS nxvk_app_example
    VERBATIM)
add_custom_target(nro ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/nxvk_app_example.nro")
