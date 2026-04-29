module enum_cast_width_fail (
    input  logic [2:0] raw,
    output logic [1:0] state_bits
);
    typedef enum logic [1:0] {
        ST_IDLE = 2'b00,
        ST_RUN  = 2'b01,
        ST_DONE = 2'b10
    } state_t;

    state_t state;

    always_comb begin
        state = state_t'(raw);
    end

    assign state_bits = state;
endmodule
