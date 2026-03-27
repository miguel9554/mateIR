// pkg_router.sv
// Features tested:
//   - Compilation-unit scope wildcard import (types_pkg::*)
//   - Duplicate wildcard import of same package — harmless, idempotent (spec §3.2)
//   - Compilation-unit scope explicit import of one name (ctrl_pkg::sched_t)
//   - ctrl_pkg enum literals NOT imported (only sched_t type is); accessed via pkg::
//   - Qualified type in port declarations (ctrl_pkg::op_t used as port type)
//   - Qualified constant references (ctrl_pkg::SCHED_RR, ctrl_pkg::OP_NOP, etc.)
import types_pkg::*;
import types_pkg::*;        // duplicate — harmless per spec §3.2
import ctrl_pkg::sched_t;   // explicit: only the type; literals still need ctrl_pkg::

module pkg_router (
    input  wire           clk,
    input  wire           rst_n,
    input  dir_t          dir_in,       // types_pkg from comp-unit wildcard
    input  ctrl_pkg::op_t op_in,        // qualified type — ctrl_pkg not wildcard imported
    input  sched_t        sched_in,     // ctrl_pkg::sched_t from comp-unit explicit import
    input  weight_t       weight_in,    // types_pkg from comp-unit wildcard
    input  logic [7:0]    data_in,
    output dir_t          dir_out,
    output ctrl_pkg::op_t op_out,       // qualified type in output port
    output logic [7:0]    routed_data
);

    dir_t         dir_reg;
    ctrl_pkg::op_t op_reg;   // qualified type for internal signal

    // Direction routing — sched_t values need ctrl_pkg:: since literals not imported
    dir_t dir_next;
    always_comb begin
        if      (sched_in == ctrl_pkg::SCHED_RR)   dir_next = dir_in;
        else if (sched_in == ctrl_pkg::SCHED_PRIO)  dir_next = types_pkg::DIR_S; // qualified even when wildcard-imported
        else if (sched_in == ctrl_pkg::SCHED_FIFO)  dir_next = DIR_W;            // unqualified (wildcard)
        else if (weight_in == WEIGHT_MAX)            dir_next = DIR_S;            // WEIGHT_MAX from wildcard
        else                                         dir_next = DIR_E;
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dir_reg <= DIR_N;                   // DIR_N from comp-unit wildcard
            op_reg  <= ctrl_pkg::OP_NOP;        // qualified constant (ctrl_pkg not fully imported)
        end else begin
            dir_reg <= dir_next;
            // Qualified constant comparison; types_pkg WEIGHT_LIGHT via wildcard
            op_reg  <= (weight_in == WEIGHT_LIGHT) ? ctrl_pkg::OP_NOP : op_in;
        end
    end

    // Data routing based on registered direction — uses wildcard-imported constants
    logic [7:0] route_result;
    assign route_result = (dir_reg == DIR_N) ? data_in :
                          (dir_reg == DIR_E) ? {data_in[3:0], data_in[7:4]} :
                          (dir_reg == DIR_S) ? ~data_in :
                          data_in ^ 8'hFF;

    // Gate data by op: qualified constant comparison
    logic [7:0] data_reg;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) data_reg <= 8'h00;
        else        data_reg <= (op_reg == ctrl_pkg::OP_NOP)   ? 8'h00 :
                                (op_reg == ctrl_pkg::OP_STORE)  ? data_in :
                                route_result;
    end

    assign dir_out     = dir_reg;
    assign op_out      = op_reg;
    assign routed_data = data_reg;

endmodule
