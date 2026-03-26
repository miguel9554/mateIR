// Single-module enum test with all enum typedefs local to the module.
// No enum types appear in the port list.
// Tests:
//   - local typedef enums (7 distinct types)
//   - enum flops
//   - enum comparisons feeding arithmetic and mux selectors
//   - case statements on every enum type
//   - enum as both mux selector and mux data
//   - enum != and == comparisons gating logic signals
//   - enum localparams
//   - mixed arithmetic + enum datapath
//   - enum-driven FSM with arithmetic accumulator
module enum_local_test (
    input  wire        clk,
    input  wire        rst_n,
    // All ports are plain logic — enums live entirely inside the module
    input  logic [1:0] op_in,      // maps to op_t internally
    input  logic [1:0] mode_in,    // maps to mode_t internally
    input  logic [1:0] shift_in,   // maps to shift_t internally
    input  logic [1:0] prio_in,    // maps to prio_t internally
    input  logic [7:0] data_a,
    input  logic [7:0] data_b,
    output logic [7:0] result_out,
    output logic [7:0] accum_out,
    output logic [2:0] state_out,  // underlying bits of state_t
    output logic [1:0] status_out, // underlying bits of status_t
    output logic [1:0] color_out,  // underlying bits of color_t
    output logic       valid_out,
    output logic       overflow_out
);

    // -----------------------------------------------------------------------
    // Local enum typedefs — none exported through ports
    // -----------------------------------------------------------------------

    typedef enum logic [1:0] {
        OP_NOP  = 2'b00,
        OP_ADD  = 2'b01,
        OP_SUB  = 2'b10,
        OP_AND  = 2'b11
    } op_t;

    typedef enum logic [1:0] {
        MODE_PASS  = 2'b00,
        MODE_INV   = 2'b01,
        MODE_XOR   = 2'b10,
        MODE_SHIFT = 2'b11
    } mode_t;

    typedef enum logic [1:0] {
        SHIFT_1 = 2'b00,
        SHIFT_2 = 2'b01,
        SHIFT_4 = 2'b10,
        SHIFT_8 = 2'b11   // saturates to >>7 internally
    } shift_t;

    typedef enum logic [1:0] {
        PRIO_LOW  = 2'b00,
        PRIO_MED  = 2'b01,
        PRIO_HIGH = 2'b10,
        PRIO_CRIT = 2'b11
    } prio_t;

    typedef enum logic [2:0] {
        ST_IDLE  = 3'b000,
        ST_LOAD  = 3'b001,
        ST_EXEC  = 3'b010,
        ST_ACCUM = 3'b011,
        ST_DONE  = 3'b100,
        ST_ERR   = 3'b101
    } state_t;

    typedef enum logic [1:0] {
        STATUS_OK   = 2'b00,
        STATUS_OVFL = 2'b01,
        STATUS_ZERO = 2'b10,
        STATUS_ERR  = 2'b11
    } status_t;

    typedef enum logic [1:0] {
        COLOR_RED   = 2'b00,
        COLOR_GREEN = 2'b01,
        COLOR_BLUE  = 2'b10,
        COLOR_WHITE = 2'b11
    } color_t;

    // -----------------------------------------------------------------------
    // Enum localparams
    // -----------------------------------------------------------------------
    localparam op_t     DEFAULT_OP     = OP_NOP;
    localparam status_t RESET_STATUS   = STATUS_OK;
    localparam color_t  RESET_COLOR    = COLOR_RED;
    localparam state_t  RESET_STATE    = ST_IDLE;

    // -----------------------------------------------------------------------
    // Cast inputs to enum types
    // -----------------------------------------------------------------------
    op_t    op;
    mode_t  mode;
    shift_t shift;
    prio_t  prio;

    assign op    = op_t'(op_in);
    assign mode  = mode_t'(mode_in);
    assign shift = shift_t'(shift_in);
    assign prio  = prio_t'(prio_in);

    // -----------------------------------------------------------------------
    // Arithmetic unit — case on op_t
    // -----------------------------------------------------------------------
    logic [7:0] add_result, sub_result, and_result;
    logic        ovfl_add, ovfl_sub;

    assign add_result = data_a + data_b;
    assign sub_result = data_a - data_b;
    assign and_result = data_a & data_b;

    assign ovfl_add = (data_a[7] == data_b[7]) && (add_result[7] != data_a[7]);
    assign ovfl_sub = (data_a[7] != data_b[7]) && (sub_result[7] != data_a[7]);

    logic [7:0] alu_result;
    status_t    alu_status;

    always_comb begin
        unique case (op)
            OP_NOP: begin
                alu_result = 8'b0;
                alu_status = STATUS_ZERO;
            end
            OP_ADD: begin
                alu_result = add_result;
                alu_status = ovfl_add           ? STATUS_OVFL :
                             (add_result == '0) ? STATUS_ZERO :
                             STATUS_OK;
            end
            OP_SUB: begin
                alu_result = sub_result;
                alu_status = ovfl_sub           ? STATUS_OVFL :
                             (sub_result == '0) ? STATUS_ZERO :
                             STATUS_OK;
            end
            OP_AND: begin
                alu_result = and_result;
                alu_status = (and_result == '0) ? STATUS_ZERO : STATUS_OK;
            end
            default: begin
                alu_result = 8'hFF;
                alu_status = STATUS_ERR;
            end
        endcase
    end

    // -----------------------------------------------------------------------
    // Shifter — case on shift_t enum selects shift amount
    // -----------------------------------------------------------------------
    logic [7:0] shift_amount;

    always_comb begin
        unique case (shift)
            SHIFT_1: shift_amount = 8'd1;
            SHIFT_2: shift_amount = 8'd2;
            SHIFT_4: shift_amount = 8'd4;
            SHIFT_8: shift_amount = 8'd7;  // cap at 7 to stay in-range
            default: shift_amount = 8'd1;
        endcase
    end

    // -----------------------------------------------------------------------
    // Mode adjustment — case on mode_t, enum as mux selector and data
    // -----------------------------------------------------------------------
    logic [7:0] mode_result;

    always_comb begin
        unique case (mode)
            MODE_PASS:  mode_result = alu_result;
            MODE_INV:   mode_result = ~alu_result;
            MODE_XOR:   mode_result = alu_result ^ data_a;
            MODE_SHIFT: mode_result = alu_result >> shift_amount;
            default:    mode_result = alu_result;
        endcase
    end

    // -----------------------------------------------------------------------
    // Priority gate — prio_t enum controls output suppression
    // -----------------------------------------------------------------------
    logic [7:0] prio_mask;

    assign prio_mask = (prio == PRIO_CRIT) ? 8'hFF :
                       (prio == PRIO_HIGH) ? 8'h0F :
                       (prio == PRIO_MED)  ? 8'h03 :
                       8'h00;

    logic [7:0] gated_result;
    assign gated_result = (prio == PRIO_CRIT) ? 8'h00 : (mode_result & ~prio_mask);

    // -----------------------------------------------------------------------
    // FSM — state_t flop with 6 states
    // -----------------------------------------------------------------------
    state_t state, next_state;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) state <= RESET_STATE;
        else        state <= next_state;
    end

    always_comb begin
        unique case (state)
            ST_IDLE:  next_state = (op != DEFAULT_OP)          ? ST_LOAD  : ST_IDLE;
            ST_LOAD:  next_state =                               ST_EXEC;
            ST_EXEC:  next_state = (alu_status == STATUS_ERR)  ? ST_ERR   :
                                   (prio == PRIO_HIGH ||
                                    prio == PRIO_CRIT)         ? ST_ACCUM : ST_DONE;
            ST_ACCUM: next_state = ST_DONE;
            ST_DONE:  next_state = ST_IDLE;
            ST_ERR:   next_state = ST_IDLE;
            default:  next_state = ST_ERR;
        endcase
    end

    // -----------------------------------------------------------------------
    // Accumulator — arithmetic mixed with enum guards
    // -----------------------------------------------------------------------
    logic [7:0] accum;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            accum <= 8'b0;
        end else begin
            if (state == ST_ACCUM)
                // Accumulate only when op is arithmetic — enum comparison gates arith
                accum <= (op == OP_ADD || op == OP_SUB) ? accum + gated_result
                                                        : accum ^ gated_result;
            else if (state == ST_IDLE && op == DEFAULT_OP)
                accum <= 8'b0;  // clear on idle NOP — enum == comparison
        end
    end

    // -----------------------------------------------------------------------
    // Color flop — driven by combinations of enum comparisons
    // -----------------------------------------------------------------------
    color_t color;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            color <= RESET_COLOR;
        end else begin
            if (prio == PRIO_CRIT)
                color <= COLOR_RED;
            else if (alu_status == STATUS_OVFL)
                color <= COLOR_WHITE;
            else if (state == ST_DONE && op != OP_NOP)
                color <= COLOR_GREEN;
            else if (mode == MODE_INV || mode == MODE_SHIFT)
                color <= COLOR_BLUE;
            else
                color <= COLOR_RED;
        end
    end

    // -----------------------------------------------------------------------
    // Status flop — enum flop
    // -----------------------------------------------------------------------
    status_t status_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) status_reg <= RESET_STATUS;
        else        status_reg <= alu_status;
    end

    // -----------------------------------------------------------------------
    // Valid — enum comparisons gating a logic signal
    // -----------------------------------------------------------------------
    logic valid_comb;
    assign valid_comb = (op != DEFAULT_OP)          &&
                        (alu_status != STATUS_ERR)   &&
                        (state == ST_DONE || state == ST_ACCUM);

    logic valid_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) valid_reg <= 1'b0;
        else        valid_reg <= valid_comb;
    end

    // -----------------------------------------------------------------------
    // Overflow — chained enum comparisons
    // -----------------------------------------------------------------------
    logic overflow_comb;
    assign overflow_comb = (alu_status == STATUS_OVFL) ||
                           (status_reg == STATUS_OVFL && state != ST_IDLE);

    // -----------------------------------------------------------------------
    // Additional combinational paths mixing enums and arithmetic
    // -----------------------------------------------------------------------

    // Enum-comparison flag feeds an arithmetic expression
    logic [7:0] bias;
    assign bias = (op == OP_ADD) ? 8'h10 :
                  (op == OP_SUB) ? 8'hF0 :
                  (op == OP_AND) ? 8'h0F :
                  8'h00;

    logic [7:0] biased_result;
    assign biased_result = (status_reg == STATUS_OK) ? (gated_result + bias) : gated_result;

    // State drives a final mux selecting between accumulator and biased result
    logic [7:0] final_result;
    assign final_result = (state == ST_DONE  ) ? biased_result :
                          (state == ST_ACCUM ) ? accum         :
                          (state == ST_ERR   ) ? 8'hFF         :
                          8'b0;

    // -----------------------------------------------------------------------
    // Outputs — enum signals converted to logic widths
    // -----------------------------------------------------------------------
    assign result_out   = final_result;
    assign accum_out    = accum;
    assign state_out    = state;       // state_t  → logic [2:0]
    assign status_out   = status_reg;  // status_t → logic [1:0]
    assign color_out    = color;       // color_t  → logic [1:0]
    assign valid_out    = valid_reg;
    assign overflow_out = overflow_comb;

endmodule
