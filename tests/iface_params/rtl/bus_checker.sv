module bus_checker (
    input  logic        clk,
    input  logic        rst_n,
    bus_if.consumer     bus,
    output logic [15:0] snap,
    output logic        snap_valid
);
    logic [15:0] snap_q;
    logic        v_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            snap_q <= '0;
            v_q    <= 1'b0;
        end else begin
            snap_q <= bus.data + 16'd1;
            v_q    <= bus.valid;
        end
    end

    assign snap       = snap_q;
    assign snap_valid = v_q;
endmodule
