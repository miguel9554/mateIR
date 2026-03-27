// Testbench for pkg_scope_test.
// Drives all inputs — no checking.
// Goal: maximum toggling across all enum and data inputs to exercise every
// package-scoping feature in the RTL.
import types_pkg::*;
import ctrl_pkg::*;

module uut_tb(
    uut_if.master _if
);

    // -----------------------------------------------------------------------
    // Clock: 10 ns period
    // -----------------------------------------------------------------------
    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    // -----------------------------------------------------------------------
    // Reset
    // -----------------------------------------------------------------------
    initial begin
        _if.rst_n = 1;
        @(posedge _if.clk);
        #1ns _if.rst_n = 0;
        repeat (4) @(posedge _if.clk);
        #1ns _if.rst_n = 1;
    end

    // -----------------------------------------------------------------------
    // Stimulus — all drives via NBA inside @(posedge clk) blocks
    // -----------------------------------------------------------------------
    always @(posedge _if.clk) begin

        // Initialise all inputs
        @(posedge _if.clk) begin
            _if.op_in     <= OP_NOP;
            _if.src_in    <= SRC_REG;
            _if.dst_in    <= DST_REG;
            _if.sched_in  <= SCHED_RR;
            _if.dir_in    <= DIR_N;
            _if.shape_in  <= SHAPE_CIRCLE;
            _if.weight_in <= WEIGHT_LIGHT;
            _if.data_in   <= 8'h00;
        end

        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        @(posedge _if.clk);

        // -------------------------------------------------------------------
        // Phase 1: walk all 8 op values combined with cycling src/dst
        // Exercises: op_t, src_t, dst_t from ctrl_pkg
        // -------------------------------------------------------------------
        for (int o = 0; o < 8; o++) begin
            @(posedge _if.clk) begin
                _if.op_in    <= op_t'(o[2:0]);
                _if.src_in   <= src_t'(o[1:0]);
                _if.dst_in   <= dst_t'(2'(o + 1));
                _if.data_in  <= 8'(o * 17 + 5);
                _if.dir_in   <= dir_t'(o[1:0]);
                _if.shape_in <= shape_t'(3'(o) % 3'd5);
            end
        end

        // -------------------------------------------------------------------
        // Phase 2: all dir × weight combinations
        // Exercises: dir_t, weight_t from types_pkg
        // -------------------------------------------------------------------
        for (int d = 0; d < 4; d++) begin
            for (int w = 0; w < 4; w++) begin
                @(posedge _if.clk) begin
                    _if.dir_in    <= dir_t'(d[1:0]);
                    _if.weight_in <= weight_t'(w[1:0]);
                    _if.sched_in  <= sched_t'(2'(d + w));
                    _if.op_in     <= op_t'(3'(d * 2 + w));
                    _if.data_in   <= 8'(d * 43 + w * 11 + 7);
                end
            end
        end

        // -------------------------------------------------------------------
        // Phase 3: all 5 shape values
        // Exercises: shape_t from types_pkg; shape drives size_t in datapath
        // -------------------------------------------------------------------
        for (int s = 0; s < 5; s++) begin
            @(posedge _if.clk) begin
                _if.shape_in <= shape_t'(s[2:0]);
                _if.op_in    <= op_t'(3'(s * 3));
                _if.src_in   <= src_t'(s[1:0]);
                _if.data_in  <= 8'(s * 53 + 3);
            end
        end

        // -------------------------------------------------------------------
        // Phase 4: all sched × dst combinations
        // Exercises: sched_t (ctrl_pkg), dst_t (ctrl_pkg), dir routing
        // -------------------------------------------------------------------
        for (int sc = 0; sc < 4; sc++) begin
            for (int ds = 0; ds < 4; ds++) begin
                @(posedge _if.clk) begin
                    _if.sched_in <= sched_t'(sc[1:0]);
                    _if.dst_in   <= dst_t'(ds[1:0]);
                    _if.op_in    <= op_t'(3'(sc + ds));
                    _if.dir_in   <= dir_t'(2'(sc ^ ds));
                    _if.data_in  <= 8'(sc * 37 + ds * 19 + 1);
                end
            end
        end

        // -------------------------------------------------------------------
        // Phase 5: WEIGHT_MAX and WEIGHT_HEAVY — exercises router gating
        // -------------------------------------------------------------------
        @(posedge _if.clk) begin
            _if.weight_in <= WEIGHT_MAX;
        end
        for (int o = 0; o < 8; o++) begin
            @(posedge _if.clk) begin
                _if.op_in    <= op_t'(o[2:0]);
                _if.dir_in   <= dir_t'(o[1:0]);
                _if.sched_in <= sched_t'(o[1:0]);
                _if.data_in  <= 8'hFF - 8'(o);
            end
        end
        @(posedge _if.clk) begin
            _if.weight_in <= WEIGHT_HEAVY;
        end
        for (int o = 0; o < 8; o++) begin
            @(posedge _if.clk) begin
                _if.op_in    <= op_t'(o[2:0]);
                _if.src_in   <= src_t'(o[1:0]);
                _if.dst_in   <= dst_t'(2'(~o));
                _if.data_in  <= 8'(o * 29 + 1);
            end
        end

        // -------------------------------------------------------------------
        // Phase 6: exercise DST_NULL gating path; op_in = OP_STORE path
        // -------------------------------------------------------------------
        @(posedge _if.clk) begin
            _if.weight_in <= WEIGHT_LIGHT;
            _if.dst_in    <= DST_NULL;
        end
        for (int d = 0; d < 4; d++) begin
            @(posedge _if.clk) begin
                _if.op_in   <= op_t'(d[2:0]);
                _if.dir_in  <= dir_t'(d[1:0]);
                _if.data_in <= 8'(d * 63 + 9);
            end
        end
        @(posedge _if.clk) begin
            _if.dst_in  <= DST_IO;
            _if.op_in   <= OP_STORE;
        end
        repeat (4) begin
            @(posedge _if.clk) begin
                _if.data_in  <= ~_if.data_in;
                _if.dir_in   <= dir_t'(~2'(_if.dir_in));
            end
        end

        // -------------------------------------------------------------------
        // Phase 7: pseudo-random LFSR sweep — exercises all enum state paths
        // -------------------------------------------------------------------
        begin
            logic [31:0] lfsr;
            lfsr = 32'hDEAD_0001;
            repeat (300) begin
                @(posedge _if.clk) begin
                    lfsr         <= {1'b0, lfsr[31:1]} ^ (lfsr[0] ? 32'hB400_0000 : 32'h0);
                    _if.op_in    <= op_t'(lfsr[2:0]);
                    _if.src_in   <= src_t'(lfsr[4:3]);
                    _if.dst_in   <= dst_t'(lfsr[6:5]);
                    _if.sched_in <= sched_t'(lfsr[8:7]);
                    _if.dir_in   <= dir_t'(lfsr[10:9]);
                    _if.weight_in <= weight_t'(lfsr[12:11]);
                    _if.shape_in <= shape_t'(3'(lfsr[15:13]) % 3'd5);
                    _if.data_in  <= lfsr[23:16];
                end
            end
        end

        // -------------------------------------------------------------------
        // Phase 8: rapid toggling — stress transitions in all submodules
        // -------------------------------------------------------------------
        repeat (60) begin
            @(posedge _if.clk) begin
                _if.op_in    <= op_t'(~3'(_if.op_in));
                _if.dir_in   <= dir_t'(~2'(_if.dir_in));
                _if.weight_in <= weight_t'(~2'(_if.weight_in));
                _if.sched_in <= sched_t'(~2'(_if.sched_in));
                _if.data_in  <= ~_if.data_in;
                _if.data_in  <= _if.data_in + 8'h37;
            end
        end

        // Let pipeline drain
        repeat (10) @(posedge _if.clk);
        $finish;
    end

endmodule
