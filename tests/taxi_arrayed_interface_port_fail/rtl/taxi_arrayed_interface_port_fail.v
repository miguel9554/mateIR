interface taxi_arrayed_port_if;
    logic valid;
    logic ready;

    modport mst(output valid, input ready);
endinterface

module taxi_arrayed_interface_child #(
    parameter M_CNT = 2
) (
    input  logic clk,
    input  logic rst_n,
    taxi_arrayed_port_if.mst m_bus[M_CNT],
    output logic seen_child
);
    logic seen_q;
    logic pulse_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            seen_q <= 1'b0;
        end else begin
            seen_q <= m_bus[0].ready;
        end
    end

    always_ff @(posedge clk) begin
        pulse_q <= m_bus[1].ready;
    end

    assign m_bus[0].valid = pulse_q;
    assign m_bus[1].valid = seen_q;
    assign seen_child = seen_q;
endmodule

module taxi_arrayed_interface_port_fail (
    input  logic clk,
    input  logic rst_n,
    input  logic ready0,
    input  logic ready1,
    output logic seen
);
    taxi_arrayed_port_if m_bus[2]();

    logic seen_child;
    logic seen_q;

    assign m_bus[0].ready = ready0;
    assign m_bus[1].ready = ready1;

    taxi_arrayed_interface_child #(
        .M_CNT(2)
    ) u_child (
        .clk(clk),
        .rst_n(rst_n),
        .m_bus(m_bus),
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
