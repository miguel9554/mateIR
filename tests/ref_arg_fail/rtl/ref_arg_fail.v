module ref_arg_fail (
    input  wire        clk,
    input  wire        rst_n,
    input  logic [7:0] a,
    output logic [7:0] result
);
    // Task with ref arg — not supported
    task automatic negate_in_place(ref logic [7:0] x);
        x = ~x;
    endtask

    logic [7:0] comb_result;

    always_comb begin
        comb_result = a;
        negate_in_place(comb_result);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) result <= 8'h00;
        else        result <= comb_result;
    end

endmodule
