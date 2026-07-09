`timescale 1ns/1ps

module tb;
    logic [7:0] a;
    logic [7:0] b;
    logic [7:0] y;

    localparam string base_dir = "../custom-sim/stimuli";

    taxi_clockless_dpi_top_fail uut (
        .a(a),
        .b(b),
        .y(y)
    );

    recorder #(
        .filepath({base_dir, "/a.txt"}),
        .TYPE(logic [7:0]),
        .IS_SYNC(0),
        .LENGTH(1)
    ) u_a_recorder (
        .clk('0),
        .data('{a})
    );

    recorder #(
        .filepath({base_dir, "/b.txt"}),
        .TYPE(logic [7:0]),
        .IS_SYNC(0),
        .LENGTH(1)
    ) u_b_recorder (
        .clk('0),
        .data('{b})
    );

    tb_common u_tb_common();

    initial begin
        a = 8'h00;
        #3 a = 8'h5A;
        #7 a = 8'hC3;
        #11 a = 8'h3C;
        #13 a = 8'hF0;
        #17 a = 8'h0F;
        #19 $finish;
    end

    initial begin
        b = 8'h00;
        #5 b = 8'hA5;
        #7 b = 8'h69;
        #11 b = 8'h96;
        #13 b = 8'h55;
        #17 b = 8'hAA;
    end
endmodule
