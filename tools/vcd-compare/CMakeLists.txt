cmake_minimum_required(VERSION 3.20)
project(VcdCompare VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -pedantic)
endif()

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VCD_PARSER_SRC "${REPO_ROOT}/external/verilog-vcd-parser/src")
set(VCD_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/vcd-parser-gen)
file(MAKE_DIRECTORY ${VCD_GEN_DIR})

BISON_TARGET(VCDParser
    ${VCD_PARSER_SRC}/VCDParser.ypp
    ${VCD_GEN_DIR}/VCDParser.cpp
    DEFINES_FILE ${VCD_GEN_DIR}/VCDParser.hpp
)

FLEX_TARGET(VCDScanner
    ${VCD_PARSER_SRC}/VCDScanner.l
    ${VCD_GEN_DIR}/VCDScanner.cpp
)

ADD_FLEX_BISON_DEPENDENCY(VCDScanner VCDParser)

add_executable(vcd_compare
    vcd_compare.cpp
    ${VCD_PARSER_SRC}/VCDFile.cpp
    ${VCD_PARSER_SRC}/VCDFileParser.cpp
    ${VCD_PARSER_SRC}/VCDValue.cpp
    ${BISON_VCDParser_OUTPUTS}
    ${FLEX_VCDScanner_OUTPUTS}
)

target_include_directories(vcd_compare PRIVATE
    ${VCD_PARSER_SRC}
    ${VCD_GEN_DIR}
)

set_target_properties(vcd_compare PROPERTIES OUTPUT_NAME vcd-compare)

if(ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ENABLE_SANITIZERS requires GCC or Clang")
    endif()
    target_compile_options(vcd_compare PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
    target_link_options(vcd_compare PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
endif()

# Bison's %name-prefix "VCDParser" makes the parser call VCDParserlex(),
# but the scanner's YY_DECL macro defines the function as yylex().
# Bridge the mismatch with a define.
target_compile_definitions(vcd_compare PRIVATE VCDParserlex=yylex)
