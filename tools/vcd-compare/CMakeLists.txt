find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

set(VCD_PARSER_SRC ${CMAKE_SOURCE_DIR}/external/verilog-vcd-parser/src)
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

# Bison's %name-prefix "VCDParser" makes the parser call VCDParserlex(),
# but the scanner's YY_DECL macro defines the function as yylex().
# Bridge the mismatch with a define.
target_compile_definitions(vcd_compare PRIVATE VCDParserlex=yylex)
