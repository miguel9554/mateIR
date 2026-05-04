module struct_literal_positional (
    input wire clk,
    input wire rst_n,
    output logic out_a,
    output logic [3:0] out_b
);
    typedef struct packed {
        logic a;
        logic [3:0] b;
    } item_t;

    item_t s;

    always_comb begin
        s = '{1'b1, 4'hA};
        out_a = s.a;
        out_b = s.b;
    end
endmodule
