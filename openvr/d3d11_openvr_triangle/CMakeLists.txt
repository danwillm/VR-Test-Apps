cmake_minimum_required(VERSION 3.24)

project(d3d11_openvr_triangle LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This sample uses Direct3D 11 and therefore only builds on Windows.")
endif()

include(FetchContent)

# Build OpenVR from source so the example has no manually-installed SDK dependency.
# v2.15.6 is the current OpenVR SDK release at the time this example was written.
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG v2.15.6
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_MakeAvailable(openvr)

add_executable(d3d11_openvr_triangle main.cpp)

target_compile_features(d3d11_openvr_triangle PRIVATE cxx_std_17)

# OpenVR names its 64-bit Windows static target openvr_api64.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OPENVR_TARGET openvr_api64)
else()
    set(OPENVR_TARGET openvr_api)
endif()

# openvr.h otherwise declares the API entry points as dllimport on Windows.
target_compile_definitions(d3d11_openvr_triangle PRIVATE OPENVR_BUILD_STATIC)

target_link_libraries(
    d3d11_openvr_triangle
    PRIVATE
        ${OPENVR_TARGET}
        d3d11
        dxgi
        d3dcompiler
)
