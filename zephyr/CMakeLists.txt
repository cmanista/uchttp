# uchttp Zephyr module build (Stage 1: HTTP, Stage 2: HTTPS/TLS).
# Kept separate from the host CMakeLists.txt so the plain host/CMake build is
# untouched. Enabled by CONFIG_UCHTTP.

if(CONFIG_UCHTTP)

  set(UCHTTP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
  set(LLHTTP_DIR  ${UCHTTP_ROOT}/third_party/llhttp)

  zephyr_library()

  # Core engine (C++), optional middleware and the Zephyr socket port.
  zephyr_library_sources(
    ${UCHTTP_ROOT}/src/uchttp.cpp
    ${UCHTTP_ROOT}/src/uchttp_security.cpp
    ${UCHTTP_ROOT}/src/uchttp_app_middleware.cpp
    ${UCHTTP_ROOT}/src/port/zephyr_socket.cpp
  )

  # Stage 2: HTTPS/TLS port (ZephyrTlsAcceptor / ZephyrTlsCredentials) over
  # Zephyr's native secure sockets (Mbed TLS / PSA Crypto). Only built when
  # CONFIG_UCHTTP_TLS is enabled so HTTP-only builds stay lean.
  zephyr_library_sources_ifdef(CONFIG_UCHTTP_TLS
    ${UCHTTP_ROOT}/src/port/zephyr_tls.cpp
  )

  # llhttp parser (C).
  zephyr_library_sources(
    ${LLHTTP_DIR}/src/api.c
    ${LLHTTP_DIR}/src/http.c
    ${LLHTTP_DIR}/src/llhttp.c
  )

  zephyr_include_directories(
    ${UCHTTP_ROOT}/include
    ${UCHTTP_ROOT}/src
    ${UCHTTP_ROOT}/src/port
    ${LLHTTP_DIR}/include
  )

  # Map Kconfig options onto the library's compile-time constants. These are
  # applied globally (zephyr_compile_definitions) so the application — which
  # instantiates the HttpServer<> templates and therefore reads these macros at
  # its own compile time — sees exactly the same values as the library.
  zephyr_compile_definitions(
    UCHTTP_RX_BUFFER_SIZE=${CONFIG_UCHTTP_RX_BUFFER_SIZE}
    UCHTTP_TX_BUFFER_SIZE=${CONFIG_UCHTTP_TX_BUFFER_SIZE}
    UCHTTP_MAX_ROUTES=${CONFIG_UCHTTP_MAX_ROUTES}
    UCHTTP_MAX_CONNECTIONS=${CONFIG_UCHTTP_MAX_CONNECTIONS}
    UCHTTP_MAX_HEADER_BYTES=${CONFIG_UCHTTP_MAX_HEADER_BYTES}
    UCHTTP_MAX_BODY_BYTES=${CONFIG_UCHTTP_MAX_BODY_BYTES}
    UCHTTP_IDLE_TIMEOUT_MS=${CONFIG_UCHTTP_IDLE_TIMEOUT_MS}
  )

endif()
