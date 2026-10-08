# Compiles every compute shader to a header (g_<name> byte array) with fxc, then builds ps5cam-core.
find_program(FXC fxc REQUIRED)
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
file(GLOB SHADER_INCLUDES ${SHADER_DIR}/*.hlsli)

set(SHADER_HEADERS)
function(add_shader name file)
    set(out ${SHADER_OUT}/${name}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${FXC} /nologo /T cs_5_0 /E main /O3 /Vn g_${name} /Fh ${out} ${ARGN} ${SHADER_DIR}/${file}
        DEPENDS ${SHADER_DIR}/${file} ${SHADER_INCLUDES}
        COMMENT "fxc ${name}"
        VERBATIM)
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

add_library(ps5cam-core STATIC pipeline.cpp cameramodes.cpp ${SHADER_HEADERS})
target_include_directories(ps5cam-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ps5cam-core PUBLIC d3d11 dxgi)
