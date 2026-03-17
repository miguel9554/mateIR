module counter#(
    parameter WIDTH = 16,
    parameter RESET_VAL = 0
)(
    input wire clk,
    input logic reset,
    input logic a_rst,
    output reg [WIDTH-1:0] count,
    output wire [7:0] next_count,
    output wire next_done,
    output reg done
);

    assign next_count = count+1;

    assign next_done = count == 2**WIDTH-1;

    always @(posedge clk or posedge a_rst) begin
        if (a_rst) begin
            count <= RESET_VAL;
        end
        else begin
            if (reset) count <= 8'b0;
            else count <= next_count;
            done <= next_done;
        end
    end

endmodule
