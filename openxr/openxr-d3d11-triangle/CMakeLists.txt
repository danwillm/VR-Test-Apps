cmake_minimum_required(VERSION 3.20)

project(openxr_d3d11_triangle LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This sample uses D3D11 and only builds on Windows.")
endif()

include(FetchContent)

# We only need the generated OpenXR headers and loader.
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.63
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openxr_sdk)

add_executable(openxr_d3d11_triangle
    main.cpp
)

target_compile_features(openxr_d3d11_triangle PRIVATE cxx_std_17)

target_compile_definitions(openxr_d3d11_triangle PRIVATE
    XR_USE_GRAPHICS_API_D3D11
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(openxr_d3d11_triangle PRIVATE
    OpenXR::openxr_loader
    d3d11
    dxgi
    d3dcompiler
)

if(MSVC)
    target_compile_options(openxr_d3d11_triangle PRIVATE /W4 /permissive-)
endif()
