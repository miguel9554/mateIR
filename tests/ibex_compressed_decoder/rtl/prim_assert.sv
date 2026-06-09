`ifndef PRIM_ASSERT_SV
`define PRIM_ASSERT_SV

`ifndef ASSERT_DEFAULT_CLK
`define ASSERT_DEFAULT_CLK clk_i
`endif

`ifndef ASSERT_DEFAULT_RST
`define ASSERT_DEFAULT_RST !rst_ni
`endif

`ifndef ASSERT_ERROR
`define ASSERT_ERROR(__name)
`endif

`ifndef ASSUME_FPV
`define ASSUME_FPV(__name, __prop, __clk = `ASSERT_DEFAULT_CLK, __rst = `ASSERT_DEFAULT_RST)
`endif

`ifndef ASSERT_STATIC_IN_PACKAGE
`define ASSERT_STATIC_IN_PACKAGE(__name, __prop)
`endif

`ifndef ASSERT_IF
`define ASSERT_IF(__name, __prop, __enable, __clk = `ASSERT_DEFAULT_CLK, __rst = `ASSERT_DEFAULT_RST)
`endif

`ifndef ASSERT_KNOWN_IF
`define ASSERT_KNOWN_IF(__name, __sig, __enable, __clk = `ASSERT_DEFAULT_CLK, __rst = `ASSERT_DEFAULT_RST)
`endif

`include "prim_assert_dummy_macros.svh"
`include "prim_assert_sec_cm.svh"

`endif
