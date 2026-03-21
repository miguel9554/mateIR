module counter_top(
    input wire clk,
    input logic a_rst,
    output reg [7:0] count
);
    wire [7:0] next_count, count_plus_one;

    logic reset;
    logic next_done;
    logic done;

    counter#(.WIDTH(8), .RESET_VAL(0)) u_counter(.clock(clk), .*);

    assign reset = next_done;

    assign count_plus_one = next_count + (done ? 2 : 3);

endmodule
