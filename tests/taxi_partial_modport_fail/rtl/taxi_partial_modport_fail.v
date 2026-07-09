interface taxi_partial_modport_if;
    logic awvalid;
    logic awready;
    logic arvalid;
    logic arready;

    modport wr_mst(output awvalid, input awready);
    modport rd_slv(input arvalid, output arready);
endinterface

module taxi_partial_modport_child (
    input  logic clk,
    input  logic rst_n,
    taxi_partial_modport_if.wr_mst bus,
    output logic seen_child
);
    logic seen_q;
    logic ready_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            seen_q <= 1'b0;
        end else begin
            seen_q <= bus.awready;
        end
    end

    always_ff @(posedge clk) begin
        ready_q <= bus.awready;
    end

    assign bus.awvalid = ready_q;
    assign seen_child = seen_q;
endmodule

module taxi_partial_modport_fail (
    input  logic clk,
    input  logic rst_n,
    input  logic awready,
    output logic seen
);
    taxi_partial_modport_if bus();

    logic seen_child;
    logic seen_q;

    assign bus.awready = awready;

    taxi_partial_modport_child u_child (
        .clk(clk),
        .rst_n(rst_n),
        .bus(bus),
        .seen_child(seen_child)
    );

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            seen_q <= 1'b0;
        end else begin
            seen_q <= seen_child;
        end
    end

    assign seen = seen_q;
endmodule
