interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic [31:0] stim;
    logic [1:0] sel;

    // Outputs
    logic [31:0] rev_flat;
    logic [7:0] rev_idx0;
    logic [7:0] rev_idx1;
    logic [7:0] rev_idx2;
    logic [7:0] rev_idx3;
    logic [7:0] rev_var;

    modport master(output clk, output rst_n, output stim, output sel, input rev_flat, input rev_idx0, input rev_idx1, input rev_idx2, input rev_idx3, input rev_var);

    modport slave(input clk, input rst_n, input stim, input sel, output rev_flat, output rev_idx0, output rev_idx1, output rev_idx2, output rev_idx3, output rev_var);
endinterface
