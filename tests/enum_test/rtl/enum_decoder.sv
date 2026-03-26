import enum_pkg::*;

// Decodes a command+mode+priority into a gated command and colour.
// Tests:
//   - enum-typed ports (inputs and outputs)
//   - enum flop (cmd_reg)
//   - enum comparison in combinational logic
//   - enum as mux selector and mux data
//   - enum assignment from enum expression
module enum_decoder (
    input  wire    clk,
    input  wire    rst_n,
    input  cmd_t   cmd_in,
    input  mode_t  mode_in,
    input  prio_t  prio_in,
    output cmd_t   cmd_out,
    output color_t color_out,
    output logic   is_arith,
    output logic   is_mul
);

    // -----------------------------------------------------------------------
    // Enum flop: register the incoming command
    // -----------------------------------------------------------------------
    cmd_t cmd_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) cmd_reg <= CMD_NOP;
        else        cmd_reg <= cmd_in;
    end

    // -----------------------------------------------------------------------
    // Combinational decode flags — enum comparisons
    // -----------------------------------------------------------------------
    assign is_arith = (cmd_in == CMD_ADD) || (cmd_in == CMD_SUB) || (cmd_in == CMD_MUL);
    assign is_mul   = (cmd_in == CMD_MUL);

    // -----------------------------------------------------------------------
    // Gated command: enum as mux selector, enum values as mux data
    // -----------------------------------------------------------------------
    cmd_t gated_cmd;

    // PRIO_CRIT suppresses any command → NOP
    assign gated_cmd = (prio_in == PRIO_CRIT) ? CMD_NOP : cmd_reg;

    // MODE_C overrides to NOP regardless of registered command
    assign cmd_out = (mode_in == MODE_C) ? CMD_NOP : gated_cmd;

    // -----------------------------------------------------------------------
    // Colour combinational: nested enum mux
    // -----------------------------------------------------------------------
    color_t color_combo;

    always_comb begin
        if (prio_in == PRIO_CRIT)
            color_combo = COLOR_RED;
        else if (prio_in == PRIO_HIGH)
            color_combo = COLOR_GREEN;
        else if (mode_in == MODE_B)
            color_combo = COLOR_BLUE;
        else if (cmd_in == CMD_MUL)
            color_combo = COLOR_GREEN;
        else
            color_combo = COLOR_RED;
    end

    // -----------------------------------------------------------------------
    // Registered colour output — enum flop driven by enum expression
    // -----------------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) color_out <= COLOR_RED;
        else        color_out <= color_combo;
    end

endmodule
