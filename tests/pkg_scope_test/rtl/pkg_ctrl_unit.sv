// pkg_ctrl_unit.sv
// Features tested:
//   - Module-header wildcard import (ctrl_pkg::*)
//     → op_t, src_t, dst_t, OP_NOP, DST_NULL, SRC_IMM, etc. visible without pkg:: prefix
//   - Qualified type reference for unimported package (types_pkg::dir_t, types_pkg::size_t)
//   - Qualified constant references (ctrl_pkg::OP_NOP, types_pkg::DIR_N, types_pkg::SIZE_LARGE)
//   - Qualified type cast (types_pkg::dir_t'(expr))
module pkg_ctrl_unit
    import ctrl_pkg::*;
(
    input  wire        clk,
    input  wire        rst_n,
    input  op_t        op_in,      // op_t from module-header wildcard
    input  src_t       src_in,     // src_t from module-header wildcard
    input  dst_t       dst_in,     // dst_t from module-header wildcard
    input  logic [7:0] data_in,
    output op_t        op_out,
    output logic [7:0] result_out,
    output logic       valid_out
);

    // Qualified type reference — types_pkg has no import in this file
    types_pkg::dir_t  dir_reg;
    types_pkg::size_t size_combo;

    // Op flop with qualified constant reset values
    op_t op_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            op_reg  <= ctrl_pkg::OP_NOP;   // qualified constant (same pkg as wildcard)
            dir_reg <= types_pkg::DIR_N;   // qualified constant from unimported package
        end else begin
            op_reg  <= op_in;
            dir_reg <= types_pkg::dir_t'(data_in[1:0]);  // qualified type cast
        end
    end

    // Decode flags using module-header-imported names (no pkg:: prefix)
    logic is_arith;
    assign is_arith = (op_in == OP_ADD) || (op_in == OP_SUB);

    // Qualified constant comparison — using ctrl_pkg:: even though wildcard is active
    logic is_nop_qual;
    assign is_nop_qual = (op_in == ctrl_pkg::OP_NOP);

    // Qualified constant from unimported package
    logic going_north;
    assign going_north = (dir_reg == types_pkg::DIR_N);

    // Size decode using qualified constants from types_pkg
    assign size_combo = (data_in[7:6] == 2'b11) ? types_pkg::SIZE_LARGE :
                        (data_in[7:6] == 2'b10) ? types_pkg::SIZE_MED   :
                        (data_in[7:6] == 2'b01) ? types_pkg::SIZE_SMALL :
                        types_pkg::SIZE_TINY;

    // ALU: case on op_t using module-header-imported enum values directly
    logic [7:0] alu_res;
    always_comb begin
        case (op_in)
            OP_NOP:   alu_res = 8'h00;
            OP_LOAD:  alu_res = data_in;
            OP_STORE: alu_res = data_in;
            OP_ADD:   alu_res = data_in + 8'h01;
            OP_SUB:   alu_res = data_in - 8'h01;
            OP_AND:   alu_res = data_in & 8'hF0;
            OP_OR:    alu_res = data_in | 8'h0F;
            OP_XOR:   alu_res = data_in ^ 8'hAA;
            default:  alu_res = 8'hFF;
        endcase
    end

    // Src/dst gating using module-header-imported enum values
    logic [7:0] gated;
    assign gated = (dst_in == DST_NULL) ? 8'h00 :
                   (src_in == SRC_IMM)  ? data_in :
                   alu_res;

    // Direction-gated output mixing qualified and unqualified references
    logic [7:0] dir_masked;
    assign dir_masked = going_north ? gated : (gated ^ 8'hFF);

    // Registered outputs
    logic [7:0] result_reg;
    logic       valid_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            result_reg <= 8'h00;
            valid_reg  <= 1'b0;
        end else begin
            result_reg <= dir_masked;
            // Qualified constant in assignment condition
            valid_reg  <= (op_in != ctrl_pkg::OP_NOP);
        end
    end

    assign op_out     = op_reg;
    assign result_out = result_reg;
    assign valid_out  = valid_reg && (dst_in != DST_NULL);

endmodule
