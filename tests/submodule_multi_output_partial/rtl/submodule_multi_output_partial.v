module submodule_multi_output_partial (
    input wire clk,
    output wire [1:0] out_bus,
    output wire direct_out
);
    child_multi_out u0(
        .a(out_bus[0]),
        .b(direct_out)
    );

    assign out_bus[1] = 1'b0;
endmodule

module child_multi_out (
    output wire a,
    output wire b
);
    assign a = 1'b1;
    assign b = 1'b0;
endmodule
