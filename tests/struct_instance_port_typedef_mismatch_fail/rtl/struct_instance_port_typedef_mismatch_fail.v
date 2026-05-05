package a_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } a_t;
endpackage

package b_pkg;
    typedef struct packed {
        logic [7:0] data;
        logic       valid;
    } b_t;
endpackage

module struct_instance_port_typedef_mismatch_fail_child (
    input  a_pkg::a_t in_s
);
endmodule

module struct_instance_port_typedef_mismatch_fail (
    input  wire clk,
    input  wire rst_n
);
    b_pkg::b_t x;

    always_comb begin
        x = '{default: 0};
    end

    struct_instance_port_typedef_mismatch_fail_child u_child(
        .in_s(x)
    );
endmodule
