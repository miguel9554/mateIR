import enum_pkg::*;

// Top-level enum test with enum-typed ports and two submodules.
// Tests:
//   - enum-typed inputs and outputs at module boundary
//   - enum flops (state, color)
//   - enum as FSM next-state case selector
//   - enum comparisons in combinational logic
//   - enum as mux selector and mux data
//   - passing enums through two submodule boundaries (enum_decoder, enum_alu)
module enum_top_test (
    input  wire    clk,
    input  wire    rst_n,
    input  cmd_t   cmd_in,
    input  mode_t  mode_in,
    input  prio_t  prio_in,
    input  logic [7:0] data_a,
    input  logic [7:0] data_b,
    output state_t  state_out,
    output logic [7:0] result_out,
    output status_t status_out,
    output color_t  color_out,
    output logic    valid_out
);

    // -----------------------------------------------------------------------
    // Submodule: enum_decoder
    //   enum ports: cmd_t, mode_t, prio_t in; cmd_t, color_t out
    // -----------------------------------------------------------------------
    cmd_t   dec_cmd;
    color_t dec_color;
    logic   dec_is_arith;
    logic   dec_is_mul;

    enum_decoder u_dec (
        .clk      (clk),
        .rst_n    (rst_n),
        .cmd_in   (cmd_in),
        .mode_in  (mode_in),
        .prio_in  (prio_in),
        .cmd_out  (dec_cmd),
        .color_out(dec_color),
        .is_arith (dec_is_arith),
        .is_mul   (dec_is_mul)
    );

    // -----------------------------------------------------------------------
    // Submodule: enum_alu
    //   enum ports: cmd_t, mode_t in; status_t out
    // -----------------------------------------------------------------------
    logic [7:0] alu_result;
    status_t    alu_status;
    logic       alu_valid;

    enum_alu u_alu (
        .clk       (clk),
        .rst_n     (rst_n),
        .cmd_in    (dec_cmd),
        .mode_in   (mode_in),
        .data_a    (data_a),
        .data_b    (data_b),
        .result_out(alu_result),
        .status_out(alu_status),
        .valid_out (alu_valid)
    );

    // -----------------------------------------------------------------------
    // FSM — enum flop, case on state_t
    // -----------------------------------------------------------------------
    state_t state, next_state;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) state <= ST_IDLE;
        else        state <= next_state;
    end

    always_comb begin
        case (state)
            ST_IDLE: next_state = (cmd_in == CMD_NOP)          ? ST_IDLE : ST_LOAD;
            ST_LOAD: next_state =                                ST_EXEC;
            ST_EXEC: next_state = (alu_status == STATUS_ERR)   ? ST_ERR  : ST_DONE;
            ST_DONE: next_state = ST_IDLE;
            ST_ERR:  next_state = ST_IDLE;
            default: next_state = ST_ERR;
        endcase
    end

    // -----------------------------------------------------------------------
    // Enum-gated result mux — state_t as mux selector
    // -----------------------------------------------------------------------
    logic [7:0] gated_result;

    assign gated_result = (state == ST_DONE) ? alu_result          :
                          (state == ST_EXEC) ? {4'b0, data_a[3:0]} :
                          8'b0;

    // -----------------------------------------------------------------------
    // Priority-to-data mux — prio_t as mux selector, constants as mux data
    // -----------------------------------------------------------------------
    logic [7:0] prio_data;

    assign prio_data = (prio_in == PRIO_CRIT) ? 8'hFF :
                       (prio_in == PRIO_HIGH) ? 8'h80 :
                       (prio_in == PRIO_MED)  ? 8'h40 :
                       8'h00;

    // -----------------------------------------------------------------------
    // Color flop — driven by enum comparisons; decoder color used as mux data
    // -----------------------------------------------------------------------
    color_t color;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            color <= COLOR_RED;
        end else begin
            if (prio_in == PRIO_CRIT)
                color <= COLOR_RED;
            else if (mode_in == MODE_A)
                color <= COLOR_GREEN;
            else if (mode_in == MODE_B)
                color <= COLOR_BLUE;
            else
                color <= dec_color;
        end
    end

    // -----------------------------------------------------------------------
    // Additional combinational paths exercising enum ops
    // -----------------------------------------------------------------------

    logic arith_and_high;
    assign arith_and_high = dec_is_arith && (prio_in == PRIO_HIGH || prio_in == PRIO_CRIT);

    logic [7:0] blended;
    assign blended = arith_and_high ? (gated_result | prio_data) : (gated_result ^ prio_data);

    // -----------------------------------------------------------------------
    // Outputs
    // -----------------------------------------------------------------------
    assign state_out  = state;
    assign result_out = blended;
    assign status_out = alu_status;
    assign color_out  = color;
    assign valid_out  = alu_valid && (state == ST_DONE || state == ST_EXEC);

endmodule
