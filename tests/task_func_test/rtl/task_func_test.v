import mathlib_pkg::*;

module task_func_test (
    input  wire        clk,
    input  wire        rst_n,
    input  logic [7:0] a,
    input  logic [7:0] b,
    input  logic [7:0] c,
    input  logic [1:0] sel,
    output logic [7:0] result_out,
    output logic [7:0] diff_out,
    output logic [7:0] min_out,
    output logic [7:0] max_out,
    output logic [7:0] pop_out,
    output logic [7:0] mux_out
);

    // fn-name return + local var + calls pkg function (clamp8 from mathlib_pkg)
    function automatic logic [7:0] add_clamp(input logic [7:0] x, y);
        logic [7:0] s;
        s = x + y;
        add_clamp = clamp8(s, 8'h10, 8'hF0);
    endfunction

    // fn calling local fn (chain)
    function automatic logic [7:0] double_clamp(input logic [7:0] x, y);
        double_clamp = add_clamp(add_clamp(x, y), 8'h00);
    endfunction

    // case + return statement
    function automatic logic [7:0] mux4(input logic [7:0] x, input logic [7:0] y,
                                        input logic [7:0] p, input logic [7:0] q,
                                        input logic [1:0] s);
        case (s)
            2'b00: return x;
            2'b01: return y;
            2'b10: return p;
            default: return q;
        endcase
    endfunction

    // for-loop inside function
    function automatic logic [7:0] popcount8(input logic [7:0] x);
        logic [7:0] cnt;
        cnt = 8'h00;
        for (int i = 0; i < 8; i++) cnt = cnt + {7'h0, x[i]};
        popcount8 = cnt;
    endfunction

    // task with input + output args
    task automatic sat_byte(input logic [7:0] x, output logic [7:0] out);
        out = (x > 8'hF0) ? 8'hF0 : x;
    endtask

    // task with multiple input + output args
    task automatic minmax(input logic [7:0] x, input logic [7:0] y,
                          output logic [7:0] mn, output logic [7:0] mx);
        if (x < y) begin mn = x; mx = y; end
        else       begin mn = y; mx = x; end
    endtask

    // task calling another task
    task automatic minmax_sat(input logic [7:0] x, input logic [7:0] y,
                              output logic [7:0] mn, output logic [7:0] mx_sat);
        logic [7:0] mx;
        minmax(x, y, mn, mx);
        sat_byte(mx, mx_sat);
    endtask

    logic [7:0] comb_result;
    logic [7:0] comb_diff;
    logic [7:0] comb_min;
    logic [7:0] comb_max;
    logic [7:0] comb_pop;
    logic [7:0] comb_mux;

    always_comb begin
        comb_result = double_clamp(a, b);
        comb_diff   = mathlib_pkg::abs_diff(a, c);
        minmax_sat(a, b, comb_min, comb_max);
        comb_pop    = popcount8(c);
        comb_mux    = mux4(a, b, c, comb_pop, sel);
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            result_out <= 8'h00;
            diff_out   <= 8'h00;
            min_out    <= 8'h00;
            max_out    <= 8'h00;
            pop_out    <= 8'h00;
            mux_out    <= 8'h00;
        end else begin
            result_out <= comb_result;
            diff_out   <= comb_diff;
            min_out    <= comb_min;
            max_out    <= comb_max;
            pop_out    <= comb_pop;
            mux_out    <= comb_mux;
        end
    end

endmodule
