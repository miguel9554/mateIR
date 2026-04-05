module inout_arg_fail (
    input  wire        clk,
    input  wire        rst_n,
    input  logic [7:0] a,
    input  logic [7:0] b,
    output logic [7:0] out_a,
    output logic [7:0] out_b
);
    // Task with inout args — not supported
    task automatic swap_bytes(inout logic [7:0] x, inout logic [7:0] y);
        logic [7:0] tmp;
        tmp = x; x = y; y = tmp;
    endtask

    logic [7:0] comb_a, comb_b;

    always_comb begin
        comb_a = a;
        comb_b = b;
        swap_bytes(comb_a, comb_b);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_a <= 8'h00;
            out_b <= 8'h00;
        end else begin
            out_a <= comb_a;
            out_b <= comb_b;
        end
    end

endmodule
