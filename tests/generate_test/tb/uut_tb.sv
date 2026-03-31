// uut_tb.sv — testbench for generate_test
// Drives random stimulus to exercise all generate constructs.
// No assertions — this TB exists only to stimulate the design.

module uut_tb(
    uut_if.master _if
);
    localparam WIDTH      = 8;
    localparam NUM_LANES  = 4;

    // ----------------------------------------------------------------
    // Clock: 10 ns period
    // ----------------------------------------------------------------
    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    // ----------------------------------------------------------------
    // Async active-low reset: assert for ~30 ns then release
    // ----------------------------------------------------------------
    initial begin
        _if.rst_n = 0;
        #31 _if.rst_n = 1;
    end

    // ----------------------------------------------------------------
    // Stimulus
    // ----------------------------------------------------------------
    always @(posedge _if.clk) begin
        // Initialise all inputs
        @(posedge _if.clk) begin
            _if.data_in <= '0;
            _if.lane_en <= '0;
        end

        // Wait for reset to deassert
        wait (_if.rst_n == 1);
        repeat (2) @(posedge _if.clk);

        // Phase 1: all lanes enabled, random data — exercises g_lane, g_pack,
        //          g_parity (xor_reduce instances), g_any_default OR-tree
        repeat (20) begin
            @(posedge _if.clk) begin
                _if.data_in <= $urandom;
                _if.lane_en <= 4'b1111;
            end
        end

        // Phase 2: one lane at a time enabled — exercises lane-select paths
        for (int c = 0; c < NUM_LANES; c++) begin
            repeat (5) begin
                @(posedge _if.clk) begin
                    _if.data_in <= $urandom;
                    _if.lane_en <= 1 << c;
                end
            end
        end

        // Phase 3: push all lanes to all-ones (saturation) — exercises
        //          g_sat_special (lane 0) and g_sat_normal (lanes 1-3)
        repeat (5) begin
            @(posedge _if.clk) begin
                _if.data_in <= {NUM_LANES{8'hFF}};
                _if.lane_en <= 4'b1111;
            end
        end

        // Phase 4: disable all lanes — pipeline regs and accumulator hold
        repeat (5) begin
            @(posedge _if.clk) begin
                _if.lane_en <= 4'b0000;
            end
        end

        // Phase 5: drive 0xFF on lane 0 repeatedly — exercises accumulator
        //          wrap-around (sat_any_w, any_sat, unnamed genblk5)
        repeat (30) begin
            @(posedge _if.clk) begin
                _if.data_in <= {8'h00, 8'h00, 8'h00, 8'hFF};
                _if.lane_en <= 4'b0001;
            end
        end

        // Phase 7: wide random sweep — exercises all generate paths together
        repeat (50) begin
            @(posedge _if.clk) begin
                _if.data_in <= $urandom;
                _if.lane_en <= $urandom % 16;
            end
        end

        repeat (5) @(posedge _if.clk);
        $display("generate_test: stimulus complete");
        $finish();
    end

endmodule
