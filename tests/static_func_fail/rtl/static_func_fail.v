module static_func_fail (
    input  wire        clk,
    input  wire        rst_n,
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] result
);
    // Static (non-automatic) function — not supported
    function logic [7:0] add_bytes(input logic [7:0] x, input logic [7:0] y);
        add_bytes = x + y;
    endfunction

    logic [7:0] comb_result;

    always_comb begin
        comb_result = add_bytes(a, b);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) result <= 8'h00;
        else        result <= comb_result;
    end

endmodule
