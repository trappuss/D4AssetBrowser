cmake_minimum_required(VERSION 3.21)

# d4cloth — standalone cloth-physics diagnostic harness.
# Builds on Windows (MSVC, same vcpkg toolchain as the app) and Linux (system Qt6).
# Configure standalone:  cmake -S tools/d4cloth -B build-d4cloth
# Or add_subdirectory(tools/d4cloth) from the root CMakeLists.

if(NOT DEFINED PROJECT_NAME)
    project(d4cloth LANGUAGES CXX)
    set(D4CLOTH_STANDALONE ON)
else()
    set(D4CLOTH_STANDALONE OFF)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(ZLIB REQUIRED)
# lz4: vcpkg exports a CONFIG package; Debian ships only the library + headers.
find_package(lz4 CONFIG QUIET)
if(NOT TARGET lz4::lz4)
    find_library(LZ4_LIB NAMES lz4 liblz4 REQUIRED)
    find_path(LZ4_INC lz4.h REQUIRED)
    add_library(lz4::lz4 UNKNOWN IMPORTED)
    set_target_properties(lz4::lz4 PROPERTIES
        IMPORTED_LOCATION "${LZ4_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INC}")
endif()

get_filename_component(D4_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(d4cloth
    src/main.cpp
    src/ClothDoc.h      src/ClothDoc.cpp
    src/Coverage.h      src/Coverage.cpp
    src/AssetSource.h   src/AssetSource.cpp
    src/Scene.h         src/Scene.cpp
    src/LegacySolver.h  src/LegacySolver.cpp
    src/Solver.h        src/Solver.cpp
    src/Run.cpp

    # Reused app sources — compiled directly, never forked.
    ${D4_REPO_ROOT}/src/model/ModelParser.cpp
    ${D4_REPO_ROOT}/src/model/AnimParser.cpp
    ${D4_REPO_ROOT}/src/casc/CascReader.cpp
)

target_include_directories(d4cloth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${D4_REPO_ROOT}/src
)

target_link_libraries(d4cloth PRIVATE Qt6::Core ZLIB::ZLIB lz4::lz4)

if(MSVC)
    target_compile_options(d4cloth PRIVATE /EHsc /MP)
    target_compile_definitions(d4cloth PRIVATE NOMINMAX)
endif()
