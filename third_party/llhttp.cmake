# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Cezary Manista

# Single definition of the vendored llhttp sources, shared by the host build
# (top-level CMakeLists.txt) and the Zephyr module (zephyr/CMakeLists.txt).
# Include this file instead of listing the llhttp files in each build.
#
# Defines:
#   LLHTTP_DIR          root of the vendored llhttp (third_party/llhttp)
#   LLHTTP_SOURCES      list of .c files to compile
#   LLHTTP_INCLUDE_DIR  directory containing llhttp.h

set(LLHTTP_DIR ${CMAKE_CURRENT_LIST_DIR}/llhttp)

set(LLHTTP_SOURCES
    ${LLHTTP_DIR}/src/api.c
    ${LLHTTP_DIR}/src/http.c
    ${LLHTTP_DIR}/src/llhttp.c
)

set(LLHTTP_INCLUDE_DIR ${LLHTTP_DIR}/include)
