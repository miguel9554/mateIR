`timescale 1ns/1ps

module tb;
    // Inputs
    logic clk;
    logic rst_n;
    logic [31:0] stim;
    logic [1:0] sel;

    // Outputs
    logic [31:0] src_flat;
    logic [31:0] dst_flat;
    logic [7:0] dst_elem_const;
    logic [7:0] dst_elem_var;

    // Interface and connection to UUT
    uut_if _if();

    assign clk = _if.clk;
    assign rst_n = _if.rst_n;
    assign stim = _if.stim;
    assign sel = _if.sel;

    assign _if.src_flat = src_flat;
    assign _if.dst_flat = dst_flat;
    assign _if.dst_elem_const = dst_elem_const;
    assign _if.dst_elem_var = dst_elem_var;

    // modules
    unpacked_array_copy uut(.*);
    uut_tb uut_tb(.*);
    uut_recorder u_recorder(.*);
    tb_common u_tb_common();

endmodule
