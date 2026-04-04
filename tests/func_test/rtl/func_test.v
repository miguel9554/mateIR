// Tests: automatic function, local variable inside function, multi-arg function
module func_test (
    input  wire        clk,
    input  wire        rst_n,
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] sum_out,
    output logic [7:0] max_out
);

    // Simple 2-input add — baseline function
    function automatic logic [7:0] add8(input logic [7:0] x, y);
        add8 = x + y;
    endfunction

    // Uses a local variable — exercises subroutine_locals path
    function automatic logic [7:0] max8(input logic [7:0] x, y);
        logic [7:0] result;
        result = (x > y) ? x : y;
        max8 = result;
    endfunction

    logic [7:0] comb_sum;
    logic [7:0] comb_max;

    always_comb begin
        comb_sum = add8(a, b);
        comb_max = max8(a, b);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sum_out <= 8'h00;
            max_out <= 8'h00;
        end else begin
            sum_out <= comb_sum;
            max_out <= comb_max;
        end
    end

endmodule
