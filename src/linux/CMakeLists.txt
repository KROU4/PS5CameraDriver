# ps5cam-bokehd: the Linux bokeh service (camera -> GPU pipeline -> v4l2loopback "PS5 Camera").
# PS5CAM_VERSION comes from the root CMakeLists.

# The parts that need no camera, loopback device or GPU, shared with the tests. picture.h,
# calibschedule.h and pipeline.h's structures are header-only, so the tests need no Vulkan.
add_library(ps5cam-bokehd-base STATIC
    bokehd/config.cpp
    bokehd/calibration.cpp
    bokehd/openwatch.cpp
    bokehd/log.cpp)
target_include_directories(ps5cam-bokehd-base PUBLIC bokehd ${CMAKE_CURRENT_SOURCE_DIR}/../core)

add_executable(ps5cam-bokehd
    bokehd/main.cpp
    bokehd/daemon.cpp
    bokehd/capture.cpp
    bokehd/loopback.cpp
    bokehd/devices.cpp
    bokehd/testmode.cpp)
target_link_libraries(ps5cam-bokehd PRIVATE ps5cam-bokehd-base ps5cam-core)

enable_testing()
add_executable(ps5cam-bokehd-tests tests/bokehd_tests.cpp)
target_link_libraries(ps5cam-bokehd-tests PRIVATE ps5cam-bokehd-base)
# The installed config must spell out the defaults (checked when the installer folder is there).
set(PS5CAM_BOKEH_CONF ${CMAKE_CURRENT_SOURCE_DIR}/../../installer/linux/bokeh.conf)
if(EXISTS ${PS5CAM_BOKEH_CONF})
    target_compile_definitions(ps5cam-bokehd-tests PRIVATE PS5CAM_BOKEH_CONF="${PS5CAM_BOKEH_CONF}")
endif()
add_test(NAME ps5cam-bokehd-tests COMMAND ps5cam-bokehd-tests)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(target ps5cam-bokehd-base ps5cam-bokehd ps5cam-bokehd-tests)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endforeach()
    # Runs where the C++ runtime is older than the build machine's; glibc 2.34+ is still needed.
    target_link_options(ps5cam-bokehd PRIVATE -static-libstdc++ -static-libgcc)
endif()
