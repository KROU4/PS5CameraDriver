cmake_minimum_required(VERSION 3.25)
project(PS5CameraDriver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)

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
