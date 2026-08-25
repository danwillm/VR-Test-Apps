cmake_minimum_required(VERSION 3.20)

project(OpenVRDisplayRedirectSample LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This sample currently targets Windows because the preview path uses D3D11/DXGI.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SteamVR server drivers should be built as 64-bit. Configure with an x64 toolchain/generator.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(OPENVR_GIT_TAG "master" CACHE STRING "OpenVR git revision/tag to fetch")

# We only need openvr_driver.h. FetchContent_Populate avoids importing OpenVR's
# own CMake project (which changes global output directories and build flags).
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_Declare(
    openvr
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG        ${OPENVR_GIT_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_GetProperties(openvr)
if(NOT openvr_POPULATED)
    FetchContent_Populate(openvr)
endif()

add_library(openvr_headers INTERFACE)
target_include_directories(openvr_headers INTERFACE "${openvr_SOURCE_DIR}/headers")

set(DRIVER_NAME "display_redirect_sample")
set(DRIVER_TARGET "driver_${DRIVER_NAME}")
set(DRIVER_STAGE_DIR "${CMAKE_BINARY_DIR}/${DRIVER_NAME}")

add_library(${DRIVER_TARGET} SHARED
    src/driver.cpp
    src/preview_window.cpp
    src/preview_window.h
    src/log.h
)

target_link_libraries(${DRIVER_TARGET}
    PRIVATE
        openvr_headers
        d3d11
        dxgi
        dxguid
        user32
        gdi32
)

target_compile_definitions(${DRIVER_TARGET} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(${DRIVER_TARGET} PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(${DRIVER_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# SteamVR expects bin/win64/driver_<manifest-name>.dll.
set_target_properties(${DRIVER_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${DRIVER_TARGET}"
)

file(MAKE_DIRECTORY "${DRIVER_STAGE_DIR}/bin/win64")
file(MAKE_DIRECTORY "${DRIVER_STAGE_DIR}/resources/settings")

configure_file(driver.vrdrivermanifest "${DRIVER_STAGE_DIR}/driver.vrdrivermanifest" COPYONLY)
configure_file(resources/settings/default.vrsettings "${DRIVER_STAGE_DIR}/resources/settings/default.vrsettings" COPYONLY)
configure_file(scripts/install_driver.bat "${DRIVER_STAGE_DIR}/install_driver.bat" COPYONLY)
configure_file(scripts/uninstall_driver.bat "${DRIVER_STAGE_DIR}/uninstall_driver.bat" COPYONLY)

add_custom_command(TARGET ${DRIVER_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_STAGE_DIR}/bin/win64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${DRIVER_TARGET}>"
            "${DRIVER_STAGE_DIR}/bin/win64/$<TARGET_FILE_NAME:${DRIVER_TARGET}>"
    COMMENT "Staging SteamVR driver at ${DRIVER_STAGE_DIR}"
)

add_custom_target(stage_driver ALL DEPENDS ${DRIVER_TARGET})

message(STATUS "OpenVR revision: ${OPENVR_GIT_TAG}")
message(STATUS "Staged driver directory: ${DRIVER_STAGE_DIR}")
