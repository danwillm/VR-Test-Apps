cmake_minimum_required(VERSION 3.24)

project(vulkan_openvr_triangle LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# OpenVR's build defaults to static, which is convenient for a tiny example.
# Disable its libc++ override so Clang builds don't unexpectedly require libc++.
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(USE_LIBCXX OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openvr)

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

if(TARGET openvr_api64)
    set(OPENVR_TARGET openvr_api64)
elseif(TARGET openvr_api)
    set(OPENVR_TARGET openvr_api)
else()
    message(FATAL_ERROR "Could not find the OpenVR CMake target")
endif()

# Keep shader source files out of the source tree. They are generated into the
# build directory, compiled to SPIR-V, and loaded by the single C++ source file.
if(Vulkan_GLSLC_EXECUTABLE)
    set(GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(
        GLSLC_EXECUTABLE
        NAMES glslc
        HINTS
            "$ENV{VULKAN_SDK}/bin"
            "$ENV{VULKAN_SDK}/Bin"
    )
endif()

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "glslc was not found. Install the Vulkan SDK or otherwise put glslc on PATH.")
endif()

set(SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY "${SHADER_DIR}")

set(VERT_SOURCE "${SHADER_DIR}/triangle.vert")
set(FRAG_SOURCE "${SHADER_DIR}/triangle.frag")
set(VERT_SPV    "${SHADER_DIR}/triangle.vert.spv")
set(FRAG_SPV    "${SHADER_DIR}/triangle.frag.spv")

file(WRITE "${VERT_SOURCE}" [=[
#version 450

layout(location = 0) out vec3 vColor;

const vec2 kPositions[3] = vec2[](
    vec2( 0.0, -0.60),
    vec2( 0.60, 0.60),
    vec2(-0.60, 0.60)
);

const vec3 kColors[3] = vec3[](
    vec3(1.0, 0.1, 0.1),
    vec3(0.1, 1.0, 0.1),
    vec3(0.1, 0.3, 1.0)
);

void main()
{
    gl_Position = vec4(kPositions[gl_VertexIndex], 0.0, 1.0);
    vColor = kColors[gl_VertexIndex];
}
]=])

file(WRITE "${FRAG_SOURCE}" [=[
#version 450

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 oColor;

void main()
{
    oColor = vec4(vColor, 1.0);
}
]=])

add_custom_command(
    OUTPUT "${VERT_SPV}"
    COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.0
            -fshader-stage=vert "${VERT_SOURCE}" -o "${VERT_SPV}"
    DEPENDS "${VERT_SOURCE}"
    VERBATIM
)

add_custom_command(
    OUTPUT "${FRAG_SPV}"
    COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.0
            -fshader-stage=frag "${FRAG_SOURCE}" -o "${FRAG_SPV}"
    DEPENDS "${FRAG_SOURCE}"
    VERBATIM
)

add_custom_target(vulkan_openvr_triangle_shaders
    DEPENDS "${VERT_SPV}" "${FRAG_SPV}"
)

add_executable(vulkan_openvr_triangle main.cpp)
add_dependencies(vulkan_openvr_triangle vulkan_openvr_triangle_shaders)

target_link_libraries(vulkan_openvr_triangle PRIVATE
    Vulkan::Vulkan
    ${OPENVR_TARGET}
    Threads::Threads
)

# The shaders live in the build tree, so bake those build-time paths into this
# minimal example rather than adding executable-path discovery code.
target_compile_definitions(vulkan_openvr_triangle PRIVATE
    TRIANGLE_VERT_SPV="${VERT_SPV}"
    TRIANGLE_FRAG_SPV="${FRAG_SPV}"
)

# We build OpenVR itself statically via FetchContent.
if(WIN32)
    target_compile_definitions(vulkan_openvr_triangle PRIVATE OPENVR_BUILD_STATIC)
endif()
