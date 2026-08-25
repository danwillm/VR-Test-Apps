cmake_minimum_required(VERSION 3.22)

project(openvr_chaperone_visualizer VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Keep every dependency pinned so builds remain reproducible. The first CMake
# configure downloads these repositories into build/_deps.
set(FETCHCONTENT_QUIET OFF)

find_package(OpenGL REQUIRED)

# -----------------------------------------------------------------------------
# SDL2
# -----------------------------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL2)

# -----------------------------------------------------------------------------
# GLEW
# -----------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)

# Use the official release archive. Unlike the Git repository checkout,
# it contains the generated headers and source files required for building.
FetchContent_Declare(
    glew
    URL
        https://github.com/nigels-com/glew/releases/download/glew-2.3.1/glew-2.3.1.tgz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR build/cmake
)

FetchContent_MakeAvailable(glew)

if(TARGET glew_s)
    target_include_directories(glew_s
        PUBLIC
            "$<BUILD_INTERFACE:${glew_SOURCE_DIR}/include>"
    )
else()
    message(FATAL_ERROR "The fetched GLEW project did not create glew_s")
endif()

if(TARGET glew)
    set_target_properties(glew PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# -----------------------------------------------------------------------------
# OpenVR
# -----------------------------------------------------------------------------
# Build the OpenVR loader from source as a static library. This avoids needing
# to copy openvr_api.dll/libopenvr_api.so beside the final executable.
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(BUILD_UNIVERSAL OFF CACHE BOOL "" FORCE)
set(USE_LIBCXX OFF CACHE BOOL "" FORCE)
set(USE_CUSTOM_LIBCXX OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG v2.15.6
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openvr)

# OpenVR calls its 64-bit Windows target openvr_api64 and its Linux target
# openvr_api. Present one stable target name to the application.
if(TARGET openvr_api64)
    set(OPENVR_TARGET openvr_api64)
elseif(TARGET openvr_api)
    set(OPENVR_TARGET openvr_api)
else()
    message(FATAL_ERROR "The fetched OpenVR project did not create an API target")
endif()

add_library(OpenVR::OpenVR ALIAS ${OPENVR_TARGET})

# The OpenVR header otherwise marks API functions as dllimport on Windows.
target_compile_definitions(${OPENVR_TARGET}
    INTERFACE $<$<PLATFORM_ID:Windows>:OPENVR_BUILD_STATIC>)

# -----------------------------------------------------------------------------
# Application
# -----------------------------------------------------------------------------
add_executable(chaperone_visualizer
    src/main.cpp)

# SDL2main must precede SDL2 when it is present, particularly for MinGW.
if(TARGET SDL2::SDL2main)
    target_link_libraries(chaperone_visualizer PRIVATE SDL2::SDL2main)
endif()

target_link_libraries(chaperone_visualizer PRIVATE
    SDL2::SDL2-static
    glew_s
    OpenGL::GL
    OpenVR::OpenVR
)

if(MSVC)
    target_compile_options(chaperone_visualizer PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(chaperone_visualizer PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT chaperone_visualizer)
