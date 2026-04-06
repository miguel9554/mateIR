interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [7:0] a;
    logic [7:0] b;
    logic [1:0] sel;

    // Outputs
    logic [7:0] div_out;
    logic [7:0] mod_out;
    logic [15:0] pow_out;
    logic [7:0] mix_out;
    logic [7:0] accum_out;

    modport master(output clk, output rst_n, output a, output b, output sel, input div_out, input mod_out, input pow_out, input mix_out, input accum_out);

    modport slave(input clk, input rst_n, input a, input b, input sel, output div_out, output mod_out, output pow_out, output mix_out, output accum_out);
endinterface
