package a_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } payload_t;
endpackage

package b_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } payload_t;
endpackage

module struct_package_port_typedef_mismatch_fail_child (
    input  a_pkg::payload_t in_s
);
endmodule

module struct_package_port_typedef_mismatch_fail (
    input wire clk,
    input wire rst_n
);
    b_pkg::payload_t x;

    always_comb begin
        x = '{default: 0};
    end

    struct_package_port_typedef_mismatch_fail_child u_child(
        .in_s(x)
    );
endmodule
