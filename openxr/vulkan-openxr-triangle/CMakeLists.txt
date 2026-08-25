cmake_minimum_required(VERSION 3.20)
project(vulkan_openxr_triangle LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

include(FetchContent)

# Keep the fetched OpenXR SDK build small.
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.61
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openxr_sdk)

add_executable(vulkan_openxr_triangle main.cpp)
target_link_libraries(vulkan_openxr_triangle PRIVATE OpenXR::openxr_loader Vulkan::Vulkan)
target_compile_definitions(vulkan_openxr_triangle PRIVATE XR_USE_GRAPHICS_API_VULKAN)

if(MSVC)
    target_compile_options(vulkan_openxr_triangle PRIVATE /W4)
else()
    target_compile_options(vulkan_openxr_triangle PRIVATE -Wall -Wextra -Wpedantic)
endif()
