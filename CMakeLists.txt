cmake_minimum_required(VERSION 3.20)
project(Mate VERSION 0.1.0 LANGUAGES CXX)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Add some useful compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -pedantic)
endif()

# Find pre-built slang.
# Debug consumers must use a Debug-built slang because SLANG_DEBUG changes ABI.
set(SLANG_DEBUG_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/external/slang-install-debug")
set(SLANG_RELEASE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/external/slang-install")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(EXISTS "${SLANG_DEBUG_PREFIX}/lib/cmake/slang/slangConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${SLANG_DEBUG_PREFIX}")
    elseif(EXISTS "/opt/slang-debug/lib/cmake/slang/slangConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "/opt/slang-debug")
    else()
        message(FATAL_ERROR
            "Debug build requires a Debug-built slang install because SLANG_DEBUG changes ABI.\n"
            "Build it with:\n"
            "  CMAKE_BUILD_TYPE=Debug bash scripts/build_slang.sh\n"
            "or use:\n"
            "  make debug\n"
        )
    endif()
elseif(EXISTS "/opt/slang/lib/cmake/slang/slangConfig.cmake")
    list(APPEND CMAKE_PREFIX_PATH "/opt/slang")
elseif(EXISTS "${SLANG_RELEASE_PREFIX}/lib/cmake/slang/slangConfig.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${SLANG_RELEASE_PREFIX}")
else()
    message(FATAL_ERROR
        "slang not found. Please build it first:\n"
        "  bash scripts/build_slang.sh"
    )
endif()
find_package(slang REQUIRED CONFIG)

# Check if yaml-cpp submodule is initialized
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/yaml-cpp/CMakeLists.txt")
    message(FATAL_ERROR
        "yaml-cpp submodule not found. Please run:\n"
        "  git submodule update --init --recursive"
    )
endif()

# Add yaml-cpp (disable tests/tools to speed up build)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/yaml-cpp)

function(mate_configure_project_target target_name)
    # Ensure no optimizations in Debug builds for reliable debugging (project only, not slang)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -O0 -g3)
    endif()

    if(ENABLE_SANITIZERS)
        target_compile_options(${target_name} PRIVATE
            -fno-omit-frame-pointer
            -fsanitize=address,undefined
        )

        get_target_property(target_type ${target_name} TYPE)
        if(target_type STREQUAL "EXECUTABLE")
            target_link_options(${target_name} PRIVATE
                -fno-omit-frame-pointer
                -fsanitize=address,undefined
            )
        endif()
    endif()
endfunction()

if(ENABLE_SANITIZERS AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "ENABLE_SANITIZERS requires GCC or Clang")
endif()

set(MATEIR_SOURCES
    src/mateir/constant_value.cpp
    src/mateir/dfg.cpp
    src/mateir/module.cpp
    src/mateir/types.cpp
)

set(SYSTEMVERILOG_FRONTEND_SOURCES
    src/frontends/systemverilog/domain_facts.cpp
    src/frontends/systemverilog/passes/cdc_annotations.cpp
    src/frontends/systemverilog/passes/cdc_check.cpp
    src/frontends/systemverilog/passes/cdc_emit.cpp
    src/frontends/systemverilog/passes/clock_reset_demands.cpp
    src/frontends/systemverilog/passes/condition_normalization.cpp
    src/frontends/systemverilog/passes/constant_fold.cpp
    src/frontends/systemverilog/passes/dce.cpp
    src/frontends/systemverilog/passes/dfg_inline.cpp
    src/frontends/systemverilog/passes/domains_propagate_and_check.cpp
    src/frontends/systemverilog/passes/elaboration.cpp
    src/frontends/systemverilog/passes/extractor.cpp
    src/frontends/systemverilog/passes/flop_resolve.cpp
    src/frontends/systemverilog/passes/global_domain_resolve.cpp
    src/frontends/systemverilog/passes/io_domains_set.cpp
    src/frontends/systemverilog/passes/sync_domain_propagate.cpp
    src/frontends/systemverilog/passes/top_io_domains_emit.cpp
    src/frontends/systemverilog/passes/top_io_domains_infer.cpp
    src/frontends/systemverilog/passes/type_propagation.cpp
    src/frontends/systemverilog/syntax_helpers.cpp
    src/frontends/systemverilog/systemverilog_frontend.cpp
    src/frontends/systemverilog/systemverilog_pipeline.cpp
    src/frontends/systemverilog/unresolved.cpp
)

set(SIM_VALUE_SOURCES
    src/sim/sim_value.cpp
)

set(RTL_RUNTIME_SOURCES
    src/sim/runtime_model.cpp
    src/sim/runtime_metadata.cpp
)

set(RTL_RUNTIME_COMPILER_SOURCES
    src/sim/runtime_compiler.cpp
)

set(ABI_NATIVE_SOURCES
    src/abi/abi_native.cpp
)

set(VECTOR_SIMULATOR_SOURCES
    tools/mate-vector-simulator/sim_engine.cpp
    tools/mate-vector-simulator/simulator_consumer.cpp
    tools/mate-vector-simulator/simulator.cpp
    tools/mate-vector-simulator/vcd_writer.cpp
)

add_library(mate-mateir STATIC ${MATEIR_SOURCES})
target_include_directories(mate-mateir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mate-mateir PRIVATE slang::slang)
mate_configure_project_target(mate-mateir)

add_library(mate-systemverilog-frontend STATIC ${SYSTEMVERILOG_FRONTEND_SOURCES})
target_include_directories(mate-systemverilog-frontend
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external/yaml-cpp/include
)
target_link_libraries(mate-systemverilog-frontend
    PUBLIC mate-mateir
    PRIVATE slang::slang yaml-cpp
)
mate_configure_project_target(mate-systemverilog-frontend)

add_library(mate-sim-value STATIC ${SIM_VALUE_SOURCES})
target_include_directories(mate-sim-value PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
mate_configure_project_target(mate-sim-value)

add_library(mate-rtl-runtime STATIC ${RTL_RUNTIME_SOURCES})
target_include_directories(mate-rtl-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mate-rtl-runtime PUBLIC mate-mateir mate-sim-value)
mate_configure_project_target(mate-rtl-runtime)

add_library(mate-rtl-runtime-compiler STATIC ${RTL_RUNTIME_COMPILER_SOURCES})
target_include_directories(mate-rtl-runtime-compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mate-rtl-runtime-compiler PUBLIC
    mate-systemverilog-frontend
    mate-rtl-runtime
)
mate_configure_project_target(mate-rtl-runtime-compiler)

# No mateir/frontend/yaml-cpp/slang dependency by design: this is the ABI
# backend linked into DPI simulation binaries (tests/common/verilator.mk),
# and Phase 2E requires those to be absent from that link.
add_library(mate-abi-native STATIC ${ABI_NATIVE_SOURCES})
target_include_directories(mate-abi-native PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
mate_configure_project_target(mate-abi-native)

add_library(mate-dpi-codegen STATIC src/dpi_codegen/dpi_codegen.cpp src/dpi_codegen/dpi_lib_link.cpp)
target_include_directories(mate-dpi-codegen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(mate-dpi-codegen PUBLIC
    mate-rtl-runtime-compiler
    Threads::Threads
    slang::slang
)
mate_configure_project_target(mate-dpi-codegen)

add_library(mate-vcd-tracer STATIC external/cpp-vcd-tracer/src/vcd_tracer.cpp)
target_include_directories(mate-vcd-tracer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-vcd-tracer/src
)
mate_configure_project_target(mate-vcd-tracer)

add_library(mate-vector-simulator-lib STATIC ${VECTOR_SIMULATOR_SOURCES})
target_include_directories(mate-vector-simulator-lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mate-vector-simulator
)
target_link_libraries(mate-vector-simulator-lib
    PUBLIC mate-rtl-runtime mate-dpi-codegen mate-vcd-tracer ${CMAKE_DL_LIBS}
)
target_compile_definitions(mate-vector-simulator-lib PRIVATE
    MATE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
mate_configure_project_target(mate-vector-simulator-lib)

add_library(mate-static-analysis STATIC src/consumers/static_analysis/static_analysis.cpp)
target_include_directories(mate-static-analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mate-static-analysis PUBLIC mate-mateir)
mate_configure_project_target(mate-static-analysis)

add_executable(mate src/main.cpp)
target_include_directories(mate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mate-vector-simulator
)
target_link_libraries(mate PRIVATE
    mate-rtl-runtime-compiler
    mate-static-analysis
    mate-vector-simulator-lib
    mate-dpi-codegen
    slang::slang
    Threads::Threads
)
mate_configure_project_target(mate)

add_executable(mate-vector-simulator tools/mate-vector-simulator/main.cpp)
target_include_directories(mate-vector-simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/mate-vector-simulator
)
target_link_libraries(mate-vector-simulator PRIVATE
    mate-rtl-runtime-compiler
    mate-vector-simulator-lib
)
mate_configure_project_target(mate-vector-simulator)

add_executable(mate-fixed-value-diff-test tests/unit/fixed_value_diff_test.cpp)
target_include_directories(mate-fixed-value-diff-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mate-fixed-value-diff-test PRIVATE mate-sim-value)
mate_configure_project_target(mate-fixed-value-diff-test)

# Print build configuration
message(STATUS "")
message(STATUS "Mate Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "")
