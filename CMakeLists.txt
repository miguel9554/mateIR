cmake_minimum_required(VERSION 3.15)
project(CustomHDL VERSION 0.1.0 LANGUAGES CXX)

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
            "  make debug-build\n"
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

# Collect all source files in src/
file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS src/*.cpp)

# VCD tracer library (compiled directly, not via its own CMake project)
set(VCD_TRACER_SOURCES external/cpp-vcd-tracer/src/vcd_tracer.cpp)

# Create the main executable
add_executable(custom_hdl_compiler ${PROJECT_SOURCES} ${VCD_TRACER_SOURCES})

# Ensure no optimizations in Debug builds for reliable debugging (project only, not slang)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(custom_hdl_compiler PRIVATE -O0 -g3)
endif()

if(ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_SANITIZERS requires GCC or Clang")
    endif()
    target_compile_options(custom_hdl_compiler PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
    target_link_options(custom_hdl_compiler PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
endif()

# Link against slang
target_link_libraries(custom_hdl_compiler PRIVATE
    slang::slang
    yaml-cpp
)

# Include directories
target_include_directories(custom_hdl_compiler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/yaml-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/cpp-vcd-tracer/src
)

# Tools
add_subdirectory(tools/vcd-compare)

# Print build configuration
message(STATUS "")
message(STATUS "Custom HDL Compiler Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "")
