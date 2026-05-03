module struct_typedef_collection (
    input wire clk,
    input wire rst_n,
    output logic out_bit
);
    typedef struct packed {
        logic a, b;
        logic [3:0] c;
    } plain_s_t;

    typedef struct packed {
        logic [7:0] data;
        plain_s_t nested;
    } packed_s_t;

    typedef struct packed {
        plain_s_t left;
        plain_s_t right;
    } nested_again_t;

    always_comb begin
        out_bit = clk ^ rst_n;
    end
endmodule
