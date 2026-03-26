import enum_pkg::*;

// Arithmetic unit whose operation is selected by a cmd_t enum.
// Tests:
//   - enum-typed ports (inputs and outputs)
//   - enum flops (status_reg)
//   - case statement on enum (cmd_in)
//   - enum as mux selector and mux data
//   - enum != comparison
//   - localparam with enum type
module enum_alu (
    input  wire        clk,
    input  wire        rst_n,
    input  cmd_t       cmd_in,
    input  mode_t      mode_in,
    input  logic [7:0] data_a,
    input  logic [7:0] data_b,
    output logic [7:0] result_out,
    output status_t    status_out,
    output logic       valid_out
);

    // Enum-typed localparams
    localparam cmd_t    IDLE_CMD    = CMD_NOP;
    localparam status_t RESET_STATUS = STATUS_OK;

    // -----------------------------------------------------------------------
    // Arithmetic
    // -----------------------------------------------------------------------
    logic [7:0] add_result, sub_result, mul_result;
    logic        ovfl_add, ovfl_sub;

    assign add_result = data_a + data_b;
    assign sub_result = data_a - data_b;
    assign mul_result = data_a[3:0] * data_b[3:0];

    // Signed overflow: inputs same sign, result different sign
    assign ovfl_add = (data_a[7] == data_b[7]) && (add_result[7] != data_a[7]);
    assign ovfl_sub = (data_a[7] != data_b[7]) && (sub_result[7] != data_a[7]);

    // -----------------------------------------------------------------------
    // Enum-selected operation — case on cmd_t enum
    // -----------------------------------------------------------------------
    logic [7:0] computed;
    status_t    raw_status;

    always_comb begin
        unique case (cmd_in)
            CMD_NOP: begin
                computed   = 8'b0;
                raw_status = STATUS_ZERO;
            end
            CMD_ADD: begin
                computed   = add_result;
                raw_status = ovfl_add           ? STATUS_OVFL :
                             (add_result == '0) ? STATUS_ZERO :
                             STATUS_OK;
            end
            CMD_SUB: begin
                computed   = sub_result;
                raw_status = ovfl_sub           ? STATUS_OVFL :
                             (sub_result == '0) ? STATUS_ZERO :
                             STATUS_OK;
            end
            CMD_MUL: begin
                computed   = mul_result;
                raw_status = (mul_result == '0) ? STATUS_ZERO : STATUS_OK;
            end
            default: begin
                computed   = 8'hFF;
                raw_status = STATUS_ERR;
            end
        endcase
    end

    // -----------------------------------------------------------------------
    // Mode adjustment — case on mode_t enum, using enum data in mux
    // -----------------------------------------------------------------------
    logic [7:0] mode_adjusted;

    always_comb begin
        unique case (mode_in)
            MODE_A:  mode_adjusted = computed;
            MODE_B:  mode_adjusted = ~computed;
            MODE_C:  mode_adjusted = computed ^ data_a;
            default: mode_adjusted = computed;
        endcase
    end

    // -----------------------------------------------------------------------
    // Registered outputs — enum flop
    // -----------------------------------------------------------------------
    logic [7:0] result_reg;
    status_t    status_reg;
    logic       valid_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            result_reg <= 8'b0;
            status_reg <= RESET_STATUS;
            valid_reg  <= 1'b0;
        end else begin
            result_reg <= mode_adjusted;
            status_reg <= raw_status;
            // valid when command is not the idle command — enum != comparison
            valid_reg  <= (cmd_in != IDLE_CMD);
        end
    end

    // -----------------------------------------------------------------------
    // Output — enum as mux selector
    // -----------------------------------------------------------------------
    // Error status forces result to all-ones; zero status forces all-zeros
    assign result_out = (status_reg == STATUS_ERR)  ? 8'hFF :
                        (status_reg == STATUS_ZERO)  ? 8'h00 :
                        result_reg;

    assign status_out = status_reg;

    // valid only when no error — enum comparison gates a logic signal
    assign valid_out  = valid_reg && (status_reg != STATUS_ERR);

endmodule
