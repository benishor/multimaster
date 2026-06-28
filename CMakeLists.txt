cmake_minimum_required(VERSION 3.20)
project(multimaster VERSION 0.3.0 LANGUAGES CXX)

if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    # CMake < 3.21 compatibility.
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL ON)
    else()
        set(PROJECT_IS_TOP_LEVEL OFF)
    endif()
endif()

option(MULTIMASTER_BUILD_TESTS    "Build multimaster tests"    ${PROJECT_IS_TOP_LEVEL})
option(MULTIMASTER_BUILD_EXAMPLES "Build multimaster examples" ${PROJECT_IS_TOP_LEVEL})
# Link example/consumer executables against static libstdc++/libgcc so the
# resulting binaries are portable across distributions (glibc stays dynamic, so
# getaddrinfo/NSS — used for static-peer hostname resolution — keeps working).
option(MULTIMASTER_STATIC_BINARIES "Statically link libstdc++/libgcc into executables" OFF)
# Transport encryption / authentication via libsodium. When ON (default) the
# library is built with PSK-based encryption; when libsodium is unavailable the
# build continues without it and a PSK-configured mesh refuses to start.
option(MULTIMASTER_ENABLE_CRYPTO "Build with libsodium transport encryption" ON)

# Always a static archive (libmultimaster.a), regardless of BUILD_SHARED_LIBS,
# so consumers statically link the library into their binary.
add_library(multimaster STATIC
    src/peer_id.cpp
    src/socket.cpp
    src/wire.cpp
    src/crypto.cpp
    src/event_loop.cpp
    src/listener.cpp
    src/peer_connection.cpp
    src/discovery.cpp
    src/gossip_router.cpp
    src/membership.cpp
    src/peer_manager.cpp
    src/mesh_impl.cpp
    src/mesh.cpp)
add_library(multimaster::multimaster ALIAS multimaster)

target_compile_features(multimaster PUBLIC cxx_std_20)
target_include_directories(multimaster
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_link_libraries(multimaster PUBLIC Threads::Threads)

# --- libsodium (optional transport encryption) ------------------------------
set(MULTIMASTER_HAVE_CRYPTO OFF)
if(MULTIMASTER_ENABLE_CRYPTO)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM IMPORTED_TARGET libsodium)
    endif()
    if(TARGET PkgConfig::SODIUM)
        target_link_libraries(multimaster PUBLIC PkgConfig::SODIUM)
        set(MULTIMASTER_HAVE_CRYPTO ON)
    else()
        find_library(SODIUM_LIBRARY sodium)
        find_path(SODIUM_INCLUDE_DIR sodium.h)
        if(SODIUM_LIBRARY AND SODIUM_INCLUDE_DIR)
            target_include_directories(multimaster PRIVATE ${SODIUM_INCLUDE_DIR})
            target_link_libraries(multimaster PUBLIC ${SODIUM_LIBRARY})
            set(MULTIMASTER_HAVE_CRYPTO ON)
        endif()
    endif()
    if(MULTIMASTER_HAVE_CRYPTO)
        # PRIVATE: only crypto.cpp needs the macro; the rest of the codebase
        # deals only in plain byte types and is crypto-agnostic.
        target_compile_definitions(multimaster PRIVATE MULTIMASTER_HAVE_CRYPTO)
    else()
        message(WARNING
            "libsodium not found: building multimaster WITHOUT transport encryption. "
            "A mesh configured with a PSK will refuse to start.")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(multimaster PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Portable-binary link flags, applied to executables when requested.
function(multimaster_link_static target)
    if(MULTIMASTER_STATIC_BINARIES AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
    endif()
endfunction()

if(MULTIMASTER_BUILD_EXAMPLES)
    add_executable(mm_chat examples/chat.cpp)
    target_link_libraries(mm_chat PRIVATE multimaster::multimaster)
    multimaster_link_static(mm_chat)

    add_executable(mm_ping examples/ping.cpp)
    target_link_libraries(mm_ping PRIVATE multimaster::multimaster)
    multimaster_link_static(mm_ping)
endif()

if(MULTIMASTER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- install / export -------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS multimaster
    EXPORT multimasterTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/multimaster DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT multimasterTargets
    FILE multimasterTargets.cmake
    NAMESPACE multimaster::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/multimaster)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/multimasterConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/multimasterConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/multimaster)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/multimasterConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/multimasterConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/multimasterConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/multimaster)
