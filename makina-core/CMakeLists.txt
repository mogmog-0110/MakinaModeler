cmake_minimum_required(VERSION 3.21)
project(makina_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Header-only, and deliberately free of any graphics or engine dependency: the whole model must
# be testable without a device (PLAN.md 3.1).
add_library(makina_core INTERFACE)
target_include_directories(makina_core INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/external")
target_compile_features(makina_core INTERFACE cxx_std_20)

enable_testing()

add_executable(roundtrip tests/roundtrip.cpp)
target_link_libraries(roundtrip PRIVATE makina_core)

add_executable(sdf_compare tests/sdf_compare.cpp)
target_link_libraries(sdf_compare PRIVATE makina_core)

add_executable(measure_compare tests/measure_compare.cpp)
target_link_libraries(measure_compare PRIVATE makina_core)

add_executable(pov_compare tests/pov_compare.cpp)
target_link_libraries(pov_compare PRIVATE makina_core)

add_executable(bsp_compare tests/bsp_compare.cpp)
target_link_libraries(bsp_compare PRIVATE makina_core)

add_executable(edit_check tests/edit_check.cpp)
target_link_libraries(edit_check PRIVATE makina_core)

add_executable(command_check tests/command_check.cpp)
target_link_libraries(command_check PRIVATE makina_core)

# Needs no scene at all: the camera is arithmetic, so its check builds its own inputs.
add_executable(camera_check tests/camera_check.cpp)
target_link_libraries(camera_check PRIVATE makina_core)

# Builds its own scene inline: picking is about the hierarchy rule, not about any one model.
add_executable(pick_check tests/pick_check.cpp)
target_link_libraries(pick_check PRIVATE makina_core)

# Data, not input handling: nothing here opens a window, so it runs everywhere.
add_executable(keymap_check tests/keymap_check.cpp)
target_link_libraries(keymap_check PRIVATE makina_core)

# The modal state machine and where its numbers land in the tree; no window involved.
add_executable(transform_check tests/transform_check.cpp)
target_link_libraries(transform_check PRIVATE makina_core)

# Which material a surface wears. Needs scenes, so it takes the fixtures like the comparisons do.
add_executable(material_check tests/material_check.cpp)
target_link_libraries(material_check PRIVATE makina_core)

# Reading POV back: what the exporter writes, the reader has to take.
add_executable(pov_import_check tests/pov_import_check.cpp)
target_link_libraries(pov_import_check PRIVATE makina_core)

# The fourth agreement: the mesh that leaves the project, against the field that stays in it.
add_executable(mesh_compare tests/mesh_compare.cpp)
target_link_libraries(mesh_compare PRIVATE makina_core)

# Not a test: writes the boundary representation out as .stl or .obj.
add_executable(makina_mesh tools/makina_mesh.cpp)
target_link_libraries(makina_mesh PRIVATE makina_core)

# Not a test: reads a POV-Ray file in, and says what it could not take.
add_executable(makina_povin tools/makina_povin.cpp)
target_link_libraries(makina_povin PRIVATE makina_core)

# Not a test: the headless command line an agent drives (describe / apply / measure).
add_executable(makina_edit tools/makina_edit.cpp)
target_link_libraries(makina_edit PRIVATE makina_core)

foreach(t roundtrip sdf_compare measure_compare pov_compare bsp_compare edit_check command_check
          camera_check pick_check keymap_check transform_check material_check mesh_compare
          pov_import_check
          makina_edit makina_mesh makina_povin)
    if(MSVC)
        # /utf-8: sources are UTF-8 but MSVC otherwise assumes the ANSI code page.
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        # nlohmann/json is vendored third-party code; its warnings are not ours to fix.
        target_compile_options(${t} PRIVATE
            /external:I "${CMAKE_CURRENT_SOURCE_DIR}/external" /external:W0)
    endif()
endforeach()

# ctest runs the checks that need nothing but this repository: our code against our code. The
# scenes are the committed fixtures, not the generated corpus, so `ctest` works on a fresh clone
# and in CI where Grasp3D and a JDK are not present.
#
# The comparisons against the Java reference are deliberately absent here. They need dumps that
# tools/*/out produces, and those are rebuilt from the .gsf files every run -- which is the only
# thing that keeps them from going stale. build-and-test.bat is where they run.
file(GLOB FIXTURE_SCENES "${CMAKE_CURRENT_SOURCE_DIR}/tests/scenes/*.makina.json")
if(FIXTURE_SCENES)
    add_test(NAME roundtrip COMMAND roundtrip ${FIXTURE_SCENES})
    add_test(NAME bsp_compare COMMAND bsp_compare ${FIXTURE_SCENES})
    add_test(NAME edit_check COMMAND edit_check ${FIXTURE_SCENES})
    add_test(NAME command_check COMMAND command_check ${FIXTURE_SCENES})
    add_test(NAME camera_check COMMAND camera_check)
    add_test(NAME pick_check COMMAND pick_check)
    add_test(NAME keymap_check COMMAND keymap_check)
    add_test(NAME transform_check COMMAND transform_check)
    add_test(NAME material_check COMMAND material_check ${FIXTURE_SCENES})
    add_test(NAME mesh_compare COMMAND mesh_compare ${FIXTURE_SCENES})
    add_test(NAME pov_import_check COMMAND pov_import_check ${FIXTURE_SCENES})
else()
    message(WARNING "tests/scenes holds no scenes; ctest will have nothing to run")
endif()
