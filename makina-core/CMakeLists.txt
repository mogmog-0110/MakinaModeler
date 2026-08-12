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

# Not a test: the headless command line an agent drives (describe / apply / measure).
add_executable(makina_edit tools/makina_edit.cpp)
target_link_libraries(makina_edit PRIVATE makina_core)

foreach(t roundtrip sdf_compare measure_compare pov_compare bsp_compare edit_check command_check
          makina_edit)
    if(MSVC)
        # /utf-8: sources are UTF-8 but MSVC otherwise assumes the ANSI code page.
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        # nlohmann/json is vendored third-party code; its warnings are not ours to fix.
        target_compile_options(${t} PRIVATE
            /external:I "${CMAKE_CURRENT_SOURCE_DIR}/external" /external:W0)
    endif()
endforeach()

# The converted Grasp3D scenes are the corpus. tools/gsf2json must have been run first.
file(GLOB SCENE_JSON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gsf2json/out/*.makina.json")
if(SCENE_JSON)
    add_test(NAME roundtrip COMMAND roundtrip ${SCENE_JSON})
else()
    message(WARNING "No converted scenes found. Run tools/gsf2json/build-and-run.bat first.")
endif()
