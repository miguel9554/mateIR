package a_pkg;
    typedef struct packed {
        logic [3:0] x;
        logic       y;
    } payload_t;
endpackage

package b_pkg;
    typedef struct packed {
        logic [3:0] x;
        logic       y;
    } payload_t;
endpackage

module struct_package_typedef_mismatch_fail (
    input wire clk,
    input wire rst_n
);
    a_pkg::payload_t a;
    b_pkg::payload_t b;

    always_comb begin
        a = b;
    end
endmodule
