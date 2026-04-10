interface uut_if;
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

    modport master(output clk, output rst_n, output stim, output sel, input src_flat, input dst_flat, input dst_elem_const, input dst_elem_var);

    modport slave(input clk, input rst_n, input stim, input sel, output src_flat, output dst_flat, output dst_elem_const, output dst_elem_var);
endinterface
