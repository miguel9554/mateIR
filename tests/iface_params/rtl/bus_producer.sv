module bus_producer #(parameter STEP = 1) (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] seed,
    bus_if.producer    bus
);
    localparam int W = bus.W;

    logic [W-1:0] acc_q;
    logic         valid_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc_q   <= '0;
            valid_q <= 1'b0;
        end else begin
            acc_q   <= acc_q + seed + STEP + bus.gain;
            valid_q <= ~valid_q;
        end
    end

    assign bus.data  = acc_q;
    assign bus.valid = valid_q;
endmodule
