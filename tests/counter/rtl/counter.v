// Simple counter module for testing the parser
module counter#(
    parameter WIDTH = 16
)(
    input wire clk,
    input logic reset,
    input logic a_rst,
    output reg [WIDTH-1:0] count,
    output reg done
);
    wire [7:0] next_count;
    reg [7:0] my_value, my_new_value, sum, test;
    reg [3:0] my_vector_flop [0:1];
    localparam MY_PARAM = 0;

    assign next_count = count+1;

    always @(posedge clk or negedge a_rst) begin
        if (~a_rst) begin
            count <= MY_PARAM;
        end
        else begin
            if (reset) count <= 8'b0;
            else count <= next_count;
            done <= (count == 2**WIDTH-1);
        end
    end

    always @(posedge clk) begin
        my_vector_flop[0] <= 4'b1010;
        my_vector_flop[1] <= my_vector_flop[0];
    end

    always_comb begin
        my_value = 42+count;
        my_value = 43+my_value;
        my_new_value = 101*count;
        sum = 99+my_value;
        test = 1;
        if (my_vector_flop[0][0]) sum = my_value-my_new_value;
        else if (count==28+4*2) begin
            test = 8;
        end
        else begin
            sum = my_value+my_new_value;
            test = 42;
        end
    end

endmodule
