module cbus_producer (
    input  logic       rst_n,
    input  logic [7:0] seed,
    cbus_if.producer   bus
);
    logic [7:0] acc_q;
    logic       valid_q;

    // Clocked by the interface's own clock member.
    always_ff @(posedge bus.clk or negedge rst_n) begin
        if (!rst_n) begin
            acc_q   <= '0;
            valid_q <= 1'b0;
        end else begin
            acc_q   <= acc_q + seed;
            valid_q <= ~valid_q;
        end
    end

    assign bus.data  = acc_q;
    assign bus.valid = valid_q;
endmodule
