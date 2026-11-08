# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uchttp_https_server)

target_sources(app PRIVATE src/main.cpp)

# uchttp public headers + the Zephyr socket/TLS port. ZEPHYR_UCHTTP_MODULE_DIR
# is set by Zephyr's module system to the root of the uchttp module.
target_include_directories(app PRIVATE
  ${ZEPHYR_UCHTTP_MODULE_DIR}/include
  ${ZEPHYR_UCHTTP_MODULE_DIR}/src/port/zephyr
  ${ZEPHYR_UCHTTP_MODULE_DIR}/third_party/llhttp/include
)
