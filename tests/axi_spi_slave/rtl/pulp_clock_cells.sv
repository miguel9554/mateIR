// Behavioral stubs for PULP platform clock cells

module pulp_clock_inverter (
    input  logic clk_i,
    output logic clk_o
);
    assign clk_o = ~clk_i;
endmodule

module pulp_clock_mux2 (
    input  logic clk0_i,
    input  logic clk1_i,
    input  logic clk_sel_i,
    output logic clk_o
);
    assign clk_o = clk_sel_i ? clk1_i : clk0_i;
endmodule
