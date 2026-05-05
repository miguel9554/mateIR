module struct_literal_default (
    input wire clk,
    input wire rst_n,
    output logic out_ok
);
    typedef struct packed {
        logic [1:0] z;
    } sub_t;

    typedef struct packed {
        logic [3:0] a;
        sub_t b;
    } item_t;

    item_t s;

    always_comb begin
        s = '{default: '0};
        out_ok = (s.a == 4'b0000) && (s.b.z == 2'b00);
    end
endmodule
