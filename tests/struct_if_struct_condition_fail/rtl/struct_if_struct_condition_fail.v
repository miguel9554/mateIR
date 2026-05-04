module struct_if_struct_condition_fail (
    input wire clk,
    input wire rst_n,
    input wire a0,
    output logic y
);
    typedef struct packed {
        logic x;
    } item_t;

    item_t s;

    always_comb begin
        s.x = a0;
        if (s) begin
            y = 1'b1;
        end else begin
            y = 1'b0;
        end
    end
endmodule
