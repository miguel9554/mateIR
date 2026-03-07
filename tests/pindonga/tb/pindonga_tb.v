`timescale 1ns/1ps

module tb;
    reg clk;
    reg rst_n;

    pindonga uut (
        .clk(clk),
        .rst_n(rst_n)
    );

endmodule
