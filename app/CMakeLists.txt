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
