cmake_minimum_required(VERSION 3.20)
project(uchttp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# llhttp
# ---------------------------------------------------------------------------
set(LLHTTP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llhttp)

add_library(llhttp STATIC
    ${LLHTTP_DIR}/src/api.c
    ${LLHTTP_DIR}/src/http.c
    ${LLHTTP_DIR}/src/llhttp.c
)
target_include_directories(llhttp PUBLIC ${LLHTTP_DIR}/include)

# ---------------------------------------------------------------------------
# uchttp  – static library (was header-only; now has uchttp.cpp)
# ---------------------------------------------------------------------------
add_library(uchttp STATIC
    src/uchttp.cpp
    src/uchttp_security.cpp
    src/uchttp_app_middleware.cpp
)
target_include_directories(uchttp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(uchttp PUBLIC llhttp)

# ---------------------------------------------------------------------------
# linux_socket  – static library (POSIX only, not for MCU targets)
# ---------------------------------------------------------------------------
add_library(linux_socket STATIC
    src/linux_socket.cpp
)
target_include_directories(linux_socket PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(linux_socket PUBLIC uchttp)

# ---------------------------------------------------------------------------
# TLS backends (requirement #4)
# ---------------------------------------------------------------------------
# OpenSSL — host builds. Always built when OpenSSL is available.
find_package(OpenSSL)
if(OpenSSL_FOUND)
    add_library(uchttp_tls_openssl STATIC
        src/openssl_tls.cpp
    )
    target_include_directories(uchttp_tls_openssl PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(uchttp_tls_openssl
        PUBLIC uchttp linux_socket OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "uchttp: OpenSSL TLS backend ENABLED (${OPENSSL_VERSION})")

    # Example HTTPS server using the OpenSSL backend.
    add_executable(uchttp_https_server src/linux_https_server.cpp)
    target_link_libraries(uchttp_https_server PRIVATE
        uchttp_tls_openssl linux_socket)
else()
    message(STATUS "uchttp: OpenSSL not found — OpenSSL TLS backend DISABLED")
endif()

# wolfSSL — embedded builds. Conditional: only when wolfSSL is present.
option(UCHTTP_WITH_WOLFSSL "Build the wolfSSL TLS backend" ON)
if(UCHTTP_WITH_WOLFSSL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WOLFSSL QUIET wolfssl)
    endif()
    if(WOLFSSL_FOUND)
        add_library(uchttp_tls_wolfssl STATIC
            src/wolfssl_tls.cpp
        )
        target_include_directories(uchttp_tls_wolfssl PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${WOLFSSL_INCLUDE_DIRS}
        )
        target_link_libraries(uchttp_tls_wolfssl
            PUBLIC uchttp linux_socket ${WOLFSSL_LIBRARIES})
        target_link_directories(uchttp_tls_wolfssl PUBLIC ${WOLFSSL_LIBRARY_DIRS})
        message(STATUS "uchttp: wolfSSL TLS backend ENABLED")
    else()
        message(STATUS "uchttp: wolfSSL not found — wolfSSL TLS backend DISABLED "
                       "(embedded target supplies it)")
    endif()
endif()

# ---------------------------------------------------------------------------
# Linux demo server
# ---------------------------------------------------------------------------
add_executable(uchttp_server
    src/linux_server.cpp
)
target_link_libraries(uchttp_server PRIVATE linux_socket)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

find_package(GTest QUIET)

if(GTest_FOUND)
    add_executable(uchttp_tests tests/test_uchttp.cpp)
    target_include_directories(uchttp_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(uchttp_tests PRIVATE uchttp GTest::gtest GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(uchttp_tests)
else()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(uchttp_tests tests/test_uchttp.cpp)
    target_include_directories(uchttp_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(uchttp_tests PRIVATE uchttp gtest gtest_main)
    include(GoogleTest)
    gtest_discover_tests(uchttp_tests)
endif()
