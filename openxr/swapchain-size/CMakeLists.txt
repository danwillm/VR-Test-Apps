cmake_minimum_required(VERSION 3.20)
project(openxr_gl_swapchain_resize_repro LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Keep dependency builds small and deterministic.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

if(UNIX AND NOT APPLE)
    # This repro intentionally targets the OpenXR OpenGL/Xlib path on Linux.
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Pin the loader/headers so the bug repro does not drift with dependency updates.
FetchContent_Declare(
    openxr_sdk
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.60.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(glfw openxr_sdk)

find_package(OpenGL REQUIRED)

add_executable(openxr_gl_swapchain_resize_repro
    src/main.cpp
)

target_link_libraries(openxr_gl_swapchain_resize_repro PRIVATE
    OpenXR::openxr_loader
    glfw
    OpenGL::GL
)

if(WIN32)
    target_compile_definitions(openxr_gl_swapchain_resize_repro PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    if(TARGET X11::X11)
        target_link_libraries(openxr_gl_swapchain_resize_repro PRIVATE X11::X11)
    else()
        target_include_directories(openxr_gl_swapchain_resize_repro PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(openxr_gl_swapchain_resize_repro PRIVATE ${X11_LIBRARIES})
    endif()
endif()

if(MSVC)
    target_compile_options(openxr_gl_swapchain_resize_repro PRIVATE /W4 /permissive-)
else()
    target_compile_options(openxr_gl_swapchain_resize_repro PRIVATE -Wall -Wextra -Wpedantic)
endif()
