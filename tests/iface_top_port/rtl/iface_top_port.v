module iface_top_port (
    input  logic       clk,
    input  logic       rst_n,
    hs_if.consumer     bus,
    output logic [7:0] acc,
    output logic       acc_valid
);
    logic [7:0] acc_q;
    logic       v_q;
    logic       ready_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            acc_q <= '0;
            v_q   <= 1'b0;
        end else if (bus.valid && ready_q) begin
            acc_q <= acc_q + bus.data;
            v_q   <= 1'b1;
        end else begin
            v_q   <= 1'b0;
        end
    end

    // Flop without async reset, fed from an interface input member.
    always_ff @(posedge clk) begin
        ready_q <= bus.valid;
    end

    assign bus.ready = ready_q;
    assign acc       = acc_q;
    assign acc_valid = v_q;
endmodule
