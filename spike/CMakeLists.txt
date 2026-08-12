cmake_minimum_required(VERSION 3.21)
project(makina_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Everything lands in one directory so the executable can find its shader blobs beside itself.
set(SPIKE_OUT "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SPIKE_OUT}")

# D-03: HLSL compiled by DXC. The spike targets DXIL only, but keeping DXC (rather than FXC)
# means Phase 8 can add -spirv without touching the shader source.
find_program(DXC_EXECUTABLE
    NAMES dxc
    HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    DOC "DirectX Shader Compiler")

if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "dxc.exe was not found. Install a Windows SDK or set DXC_EXECUTABLE.")
endif()
message(STATUS "dxc: ${DXC_EXECUTABLE}")

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SRC "${SHADER_DIR}/raymarch.hlsl")
set(SHADER_DEPS "${SHADER_SRC}" "${SHADER_DIR}/prelude.hlsl" "${SHADER_DIR}/shading.hlsl")

function(compile_shader stage entry profile out_var)
    set(out "${SPIKE_OUT}/raymarch_${stage}.cso")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPIKE_OUT}"
        COMMAND "${DXC_EXECUTABLE}" -T ${profile} -E ${entry} -O3 -I "${SHADER_DIR}"
                -Fo "${out}" "${SHADER_SRC}"
        DEPENDS ${SHADER_DEPS}
        COMMENT "dxc ${profile} ${entry} -> raymarch_${stage}.cso"
        VERBATIM)
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

compile_shader(vs VSMain vs_6_0 VS_CSO)
compile_shader(ps PSMain ps_6_0 PS_CSO)

add_custom_target(spike_shaders DEPENDS "${VS_CSO}" "${PS_CSO}")

add_executable(spike
    src/main.cpp
    src/csg_scene.hpp
    src/csg_flatten.hpp
    src/csg_codegen.hpp
    src/dx12_headless.hpp
    src/image_out.hpp)

add_dependencies(spike spike_shaders)
target_link_libraries(spike PRIVATE d3d12 dxgi dxguid)

# The generated-shader path drives dxc at runtime, so it needs the compiler and the include dir.
target_compile_definitions(spike PRIVATE
    DXC_PATH="${DXC_EXECUTABLE}"
    SPIKE_SHADER_DIR="${SHADER_DIR}")

# ── scene renderer ─────────────────────────────────────────────
# Renders a real Makina scene rather than a synthetic benchmark: json -> Scene -> program ->
# generated HLSL -> DX12 -> image.
set(MAKINA_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../makina-core")

add_executable(render_scene
    src/render_scene.cpp
    src/scene_codegen.hpp
    src/dx12_headless.hpp
    src/image_out.hpp)

target_include_directories(render_scene PRIVATE
    "${MAKINA_CORE_DIR}/include"
    "${MAKINA_CORE_DIR}/external")
target_link_libraries(render_scene PRIVATE d3d12 dxgi dxguid)
add_dependencies(render_scene spike_shaders)

# MAKINA_CORE_INCLUDE reaches the shader too: the generated HLSL includes makina/Sdf.hpp, the very
# file the CPU evaluator compiles, so both sides share one definition of each primitive (D-03).
target_compile_definitions(render_scene PRIVATE
    DXC_PATH="${DXC_EXECUTABLE}"
    SPIKE_SHADER_DIR="${SHADER_DIR}"
    MAKINA_CORE_INCLUDE="${MAKINA_CORE_DIR}/include/makina")

if(MSVC)
    target_compile_options(render_scene PRIVATE /W4 /permissive- /utf-8 /wd4324)
    target_compile_options(render_scene PRIVATE
        /external:I "${MAKINA_CORE_DIR}/external" /external:W0)
endif()

# Asset import: generates the scene's shader and compiles it to DXIL once, so a runtime does not
# need a shader compiler (SPIKE_PERF.md 9).
add_executable(makina_bake src/makina_bake.cpp src/scene_codegen.hpp src/dxc_invoke.hpp)
target_include_directories(makina_bake PRIVATE
    "${MAKINA_CORE_DIR}/include"
    "${MAKINA_CORE_DIR}/external")
add_dependencies(makina_bake spike_shaders)
target_compile_definitions(makina_bake PRIVATE
    DXC_PATH="${DXC_EXECUTABLE}"
    SPIKE_SHADER_DIR="${SHADER_DIR}"
    MAKINA_CORE_INCLUDE="${MAKINA_CORE_DIR}/include/makina")
if(MSVC)
    target_compile_options(makina_bake PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(makina_bake PRIVATE
        /external:I "${MAKINA_CORE_DIR}/external" /external:W0)
endif()

# No device and no shader: it only reads two BMPs, so it builds and runs anywhere.
add_executable(silhouette_compare src/silhouette_compare.cpp)
if(MSVC)
    target_compile_options(silhouette_compare PRIVATE /W4 /permissive- /utf-8)
endif()

# The other half of the same comparison: same shape, and now the same colors on it.
add_executable(color_compare src/color_compare.cpp)
target_include_directories(color_compare PRIVATE src)
if(MSVC)
    target_compile_options(color_compare PRIVATE /W4 /permissive- /utf-8)
endif()

if(MSVC)
    # /utf-8: sources are UTF-8 but MSVC otherwise assumes the ANSI code page (932 here) and
    # warns C4819. /wd4324 silences the expected padding from alignas(256) on the constant buffer.
    target_compile_options(spike PRIVATE /W4 /permissive- /utf-8 /wd4324)
endif()
