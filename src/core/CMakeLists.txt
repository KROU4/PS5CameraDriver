# Compiles every compute shader to a header (g_<name> byte array) and builds ps5cam-core: with fxc
# to DXBC for the Direct3D 11 backend on Windows, with dxc to SPIR-V for the Vulkan backend
# elsewhere. Both compile the same HLSL files.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
file(GLOB SHADER_INCLUDES ${SHADER_DIR}/*.hlsli)

set(PS5CAM_D3D11 OFF)
if(WIN32 AND NOT PS5CAM_VULKAN)
    set(PS5CAM_D3D11 ON)
    find_program(FXC fxc REQUIRED)
else()
    find_program(DXC dxc REQUIRED)
    find_package(Vulkan REQUIRED)
endif()

set(SHADER_HEADERS)
function(add_shader name file)
    set(out ${SHADER_OUT}/${name}.h)
    if(PS5CAM_D3D11)
        add_custom_command(OUTPUT ${out}
            COMMAND ${FXC} /nologo /T cs_5_0 /E main /O3 /Vn g_${name} /Fh ${out} ${ARGN} ${SHADER_DIR}/${file}
            DEPENDS ${SHADER_DIR}/${file} ${SHADER_INCLUDES}
            COMMENT "fxc ${name}"
            VERBATIM)
    else()
        # Registers become bindings of set 0: b0 -> 0, t# -> 10 + #, s0 -> 30, u# -> 40 + # (gpu_vulkan.cpp).
        string(REPLACE "/D" "-D" defines "${ARGN}")
        add_custom_command(OUTPUT ${out}
            COMMAND ${DXC} -nologo -spirv -T cs_6_0 -E main -O3 -fvk-use-dx-layout -fspv-target-env=vulkan1.1
                -fvk-b-shift 0 0 -fvk-t-shift 10 0 -fvk-s-shift 30 0 -fvk-u-shift 40 0
                -Vn g_${name} -Fh ${out} ${defines} ${SHADER_DIR}/${file}
            DEPENDS ${SHADER_DIR}/${file} ${SHADER_INCLUDES}
            COMMENT "dxc ${name}"
            VERBATIM)
    endif()
    set(SHADER_HEADERS ${SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

add_shader(unpack unpack.hlsl)
add_shader(downscale downscale.hlsl)
add_shader(census census.hlsl)
add_shader(aggregate aggregate.hlsl)
add_shader(wta wta.hlsl)
add_shader(lrfill lrfill.hlsl)
add_shader(holefill holefill.hlsl)
add_shader(guided_prep guided.hlsl /DENTRY_PREP)
add_shader(guided_box guided.hlsl /DENTRY_BOX)
add_shader(guided_coef guided.hlsl /DENTRY_COEF)
add_shader(histogram histogram.hlsl)
add_shader(bokeh bokeh.hlsl)
add_shader(composite composite.hlsl)
add_shader(score score.hlsl)
add_shader(lumastats lumastats.hlsl)
add_shader(denoise denoise.hlsl)
add_shader(depthout depthout.hlsl)

if(PS5CAM_D3D11)
    add_library(ps5cam-core STATIC pipeline.cpp gpu_d3d11.cpp cameramodes.cpp flicker.cpp ${SHADER_HEADERS})
    target_link_libraries(ps5cam-core PUBLIC d3d11 dxgi)
else()
    add_library(ps5cam-core STATIC pipeline.cpp gpu_vulkan.cpp flicker.cpp ${SHADER_HEADERS})
    target_link_libraries(ps5cam-core PUBLIC Vulkan::Vulkan)
    if(NOT MSVC)
        # Vulkan structures are initialised as {sType} with the rest zero, by design.
        set_source_files_properties(gpu_vulkan.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
endif()
target_include_directories(ps5cam-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
