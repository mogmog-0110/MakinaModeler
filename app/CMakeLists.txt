cmake_minimum_required(VERSION 3.21)
project(makina_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(MAKINA_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../makina-core")

# The development checkout, not the release snapshot: this is the tree the integration has to work
# against. Overridable so a different checkout can be pointed at without editing the file.
set(MITIRU_DIR "D:/sandbox/MitiruEngineDev" CACHE PATH "MitiruEngine checkout")

if(NOT EXISTS "${MITIRU_DIR}/include/mitiru/module/Reflection.hpp")
    message(FATAL_ERROR
        "MitiruEngine headers not found under '${MITIRU_DIR}'. "
        "Clone the engine or pass -DMITIRU_DIR=<path>.")
endif()

# Only the header contract is checked here, so nothing links against the engine: pulling in the
# renderer would mean CEF, vcpkg and a submodule tree for a test about struct layout.
add_executable(reflect_check reflect_check.cpp reflect_bridge.hpp)
target_include_directories(reflect_check PRIVATE
    "${MAKINA_CORE_DIR}/include"
    "${MAKINA_CORE_DIR}/external"
    "${MITIRU_DIR}/include")

if(MSVC)
    target_compile_options(reflect_check PRIVATE /W4 /permissive- /utf-8)
    # SHELL: keeps each flag with its path. Without it CMake de-duplicates and reorders the list,
    # and the second path arrives on its own -- where cl reads it as a source file.
    target_compile_options(reflect_check PRIVATE
        "SHELL:/external:I ${MAKINA_CORE_DIR}/external"
        "SHELL:/external:I ${MITIRU_DIR}/include"
        /external:W0)
endif()

# ---------------------------------------------------------------- keymap audit

# Reads the viewport back and holds the keymap to its own claim. Not linked against the viewport
# -- the viewport is a windowed program and the question here is only about its source text.
add_executable(keymap_audit keymap_audit.cpp)
target_include_directories(keymap_audit PRIVATE
    "${MAKINA_CORE_DIR}/include"
    "${MAKINA_CORE_DIR}/external")
if(MSVC)
    target_compile_options(keymap_audit PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(keymap_audit PRIVATE
        "SHELL:/external:I ${MAKINA_CORE_DIR}/external"
        /external:W0)
endif()

# ---------------------------------------------------------------- viewport

# The interactive viewport: a window, a swap chain and the same generated shader the offscreen
# renderer uses. It does not link the engine -- PLAN.md 3.1 keeps Makina and MitiruEngine
# independent, and the engine's window arrives attached to a renderer built for games.
set(SPIKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../spike")

find_program(DXC_EXECUTABLE
    NAMES dxc
    HINTS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    DOC "DirectX Shader Compiler")

if(WIN32 AND DXC_EXECUTABLE)
    add_executable(makina_viewport
        viewport/main.cpp
        viewport/win32_window.hpp
        viewport/dx12_swapchain.hpp)

    target_include_directories(makina_viewport PRIVATE
        "${MAKINA_CORE_DIR}/include"
        "${MAKINA_CORE_DIR}/external")
    target_link_libraries(makina_viewport PRIVATE d3d12 dxgi dxguid)

    # The shader source and makina-core reach the compiler at run time, not at build time: the
    # scene decides the shader, so it is generated and compiled when a model is opened.
    target_compile_definitions(makina_viewport PRIVATE
        DXC_PATH="${DXC_EXECUTABLE}"
        SPIKE_SHADER_DIR="${SPIKE_DIR}/shaders"
        MAKINA_CORE_INCLUDE="${MAKINA_CORE_DIR}/include/makina"
        # Set here rather than in a header: d3d12.h pulls in windows.h too, so whichever header
        # happens to be included first would have to carry the guard, and that is a rule about
        # include order rather than a fact about the build.
        NOMINMAX
        WIN32_LEAN_AND_MEAN)

    if(MSVC)
        target_compile_options(makina_viewport PRIVATE /W4 /permissive- /utf-8 /wd4324)
        target_compile_options(makina_viewport PRIVATE
            "SHELL:/external:I ${MAKINA_CORE_DIR}/external"
            /external:W0)
    endif()
else()
    message(STATUS "makina_viewport skipped: needs Windows and dxc")
endif()
