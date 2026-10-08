cmake_minimum_required(VERSION 3.25)
project(PS5CameraDriver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The release number, for the version resources of the Windows programs (ps5cam_version below)
# and the --version of the Linux ones.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/VERSION)
file(STRINGS ${PROJECT_SOURCE_DIR}/VERSION PS5CAM_VERSION LIMIT_COUNT 1)
string(STRIP "${PS5CAM_VERSION}" PS5CAM_VERSION)
if(NOT PS5CAM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, not '${PS5CAM_VERSION}'")
endif()

# Development: the Vulkan pipeline on Windows too (only ps5cam-core and the bench), to compare it
# with Direct3D 11 on the same GPU. Needs dxc with SPIR-V and the Vulkan headers and loader.
option(PS5CAM_VULKAN "Windows: build the pipeline on Vulkan (core and bench only)" OFF)

if(NOT WIN32 OR PS5CAM_VULKAN)
    # Linux: the GPU pipeline on Vulkan and the bokeh daemon that feeds v4l2loopback.
    if(MSVC)
        add_compile_options(/W4 /permissive- /utf-8 /EHsc)
        add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    else()
        add_compile_options(-Wall -Wextra)
    endif()
    add_compile_definitions(PS5CAM_VERSION="${PS5CAM_VERSION}")
    enable_testing()  # ctest from the build root runs the daemon's tests
    add_subdirectory(src/core)
    foreach(dir src/linux src/bench)
        if(EXISTS ${PROJECT_SOURCE_DIR}/${dir}/CMakeLists.txt AND (NOT WIN32 OR dir STREQUAL "src/bench"))
            add_subdirectory(${dir})
        endif()
    endforeach()
    return()
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

string(REPLACE "." "," PS5CAM_VERSION_COMMA "${PS5CAM_VERSION},0")

function(ps5cam_version target description)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "SHARED_LIBRARY")
        set(PS5CAM_FILETYPE VFT_DLL)
        set(PS5CAM_FILENAME ${target}.dll)
    else()
        set(PS5CAM_FILETYPE VFT_APP)
        set(PS5CAM_FILENAME ${target}.exe)
    endif()
    set(PS5CAM_NAME ${target})
    set(PS5CAM_DESCRIPTION "${description}")
    configure_file(${PROJECT_SOURCE_DIR}/src/common/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc)
endfunction()

add_subdirectory(src/service)
add_subdirectory(src/core)
add_subdirectory(src/vcam)
add_subdirectory(src/dmft)
add_subdirectory(src/ctl)
add_subdirectory(src/tray)
# Diagnostics and firmware-lab tools (developer tree only).
foreach(tool probe lab bench)
    if(EXISTS ${PROJECT_SOURCE_DIR}/src/${tool}/CMakeLists.txt)
        add_subdirectory(src/${tool})
    endif()
endforeach()
