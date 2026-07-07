interface dbus_if(input logic clk);
    logic v;
    modport m(output v, input clk);
endinterface
module dchild (input logic rst_n, dbus_if.m bus);
    logic v_r;
    always_ff @(posedge bus.clk or negedge rst_n) begin
        if (!rst_n) v_r <= 1'b0;
        else v_r <= ~v_r;
    end
    assign bus.v = v_r;
endmodule
module iface_clk_member_data_fail (
    input  logic clk,
    input  logic rst_n,
    output logic q,
    output logic bad
);
    dbus_if bus(.clk(clk));
    dchild u_c(.rst_n(rst_n), .bus(bus));
    logic q_r;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) q_r <= 1'b0;
        else q_r <= bus.v;
    end
    assign q = q_r;
    // Illegal: the interface's clock member consumed as data.
    assign bad = bus.clk;
endmodule
