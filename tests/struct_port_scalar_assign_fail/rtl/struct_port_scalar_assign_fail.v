package struct_port_scalar_assign_fail_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } payload_t;
endpackage

module struct_port_scalar_assign_fail (
    input wire clk,
    input wire rst_n
);
    struct_port_scalar_assign_fail_pkg::payload_t in_s;
    wire scalar_input;

    struct_port_scalar_assign_fail_child u_child(
        .in_s(scalar_input)
    );
endmodule

module struct_port_scalar_assign_fail_child (
    input struct_port_scalar_assign_fail_pkg::payload_t in_s
);
endmodule
