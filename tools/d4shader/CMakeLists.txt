cmake_minimum_required(VERSION 3.21)

# d4shader — shader-reuse feasibility probe.
# Standalone, exactly like tools/d4cloth: its own build dir, its own vcpkg manifest, and it
# reuses the app's readers by PATH rather than forking them.
# Configure standalone:  cmake -S tools/d4shader -B tools/d4shader/build

if(NOT DEFINED PROJECT_NAME)
    project(d4shader LANGUAGES CXX)
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

add_executable(d4shader
    src/main.cpp

    # Reused app sources — compiled directly, never forked.
    ${D4_REPO_ROOT}/src/casc/CascReader.cpp
    ${D4_REPO_ROOT}/src/index/CoreToc.cpp
    # Header-only deps (ModelGeometry.h is QString/QVector/array), so this costs nothing and keeps
    # the verified hash -> hardpoint-name table in ONE place.
    ${D4_REPO_ROOT}/src/model/Hardpoints.cpp
)

target_include_directories(d4shader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${D4_REPO_ROOT}/src
)

target_link_libraries(d4shader PRIVATE Qt6::Core ZLIB::ZLIB lz4::lz4)

if(MSVC)
    target_compile_options(d4shader PRIVATE /EHsc /MP)
    target_compile_definitions(d4shader PRIVATE NOMINMAX)
endif()
