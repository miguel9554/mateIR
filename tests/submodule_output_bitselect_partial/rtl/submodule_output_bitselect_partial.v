module submodule_output_bitselect_partial (
    input wire clk,
    input wire [3:0] in_bus,
    output wire [3:0] out_bus
);
    wire [3:0] bus;
    assign bus[0] = 1;
    assign bus[1] = in_bus[1];
    assign bus[3] = 0;
    child_out u0(.d(in_bus[0]), .q(bus[2]));
    assign out_bus = bus;
endmodule

module child_out(
    input wire d,
    output wire q
);
    assign q = d;
endmodule
