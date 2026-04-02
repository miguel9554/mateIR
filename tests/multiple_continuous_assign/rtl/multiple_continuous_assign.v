module multiple_continuous_assign(
    output wire z
);
    wire n;

    assign n = 1'b0;
    assign n = 1'b1;
    assign z = n;
endmodule
