cmake_minimum_required(VERSION 3.20)
project(multimaster VERSION 0.1.0 LANGUAGES CXX)

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

add_library(multimaster
    src/peer_id.cpp
    src/socket.cpp
    src/wire.cpp
    src/event_loop.cpp
    src/listener.cpp
    src/peer_connection.cpp
    src/discovery.cpp
    src/gossip_router.cpp
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

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(multimaster PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MULTIMASTER_BUILD_EXAMPLES)
    add_executable(mm_chat examples/chat.cpp)
    target_link_libraries(mm_chat PRIVATE multimaster::multimaster)

    add_executable(mm_ping examples/ping.cpp)
    target_link_libraries(mm_ping PRIVATE multimaster::multimaster)
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
