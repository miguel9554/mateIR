module iface_clk_member (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] seed,
    output logic [7:0] data_out,
    output logic       valid_out,
    output logic [7:0] seen_out
);
    cbus_if bus(.clk(clk));

    cbus_producer u_prod(.rst_n(rst_n), .seed(seed), .bus(bus));
    cbus_watcher  u_watch(.bus(bus), .seen(seen_out));

    logic [7:0] data_q;
    logic       valid_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            data_q  <= '0;
            valid_q <= 1'b0;
        end else begin
            data_q  <= bus.data;
            valid_q <= bus.valid;
        end
    end

    assign data_out  = data_q;
    assign valid_out = valid_q;
endmodule
