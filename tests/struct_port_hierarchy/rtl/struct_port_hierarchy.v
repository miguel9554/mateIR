package struct_port_hierarchy_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } payload_t;
endpackage

module struct_port_hierarchy (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [7:0]  in_data,
    input  wire        in_valid,
    output logic [7:0] out_data,
    output logic       out_valid
);
    struct_port_hierarchy_pkg::payload_t a;
    struct_port_hierarchy_pkg::payload_t b;

    always_comb begin
        a.data = in_data;
        a.valid = in_valid;
        out_data = b.data;
        out_valid = b.valid;
    end

    struct_port_hierarchy_child u_child(
        .in_s(a),
        .out_s(b)
    );
endmodule

module struct_port_hierarchy_child (
    input  struct_port_hierarchy_pkg::payload_t in_s,
    output struct_port_hierarchy_pkg::payload_t out_s
);
    assign out_s.data  = in_s.data + 8'h1;
    assign out_s.valid = ~in_s.valid;
endmodule
