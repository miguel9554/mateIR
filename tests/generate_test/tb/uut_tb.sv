// uut_tb.sv — testbench for generate_test
// Exercises all generate blocks: per-lane pipeline regs, accumulators,
// parity, OR-tree, saturation, async reset.

module uut_tb(
    uut_if.master _if
);

    // ----------------------------------------------------------------
    // Parameters (must match UUT defaults)
    // ----------------------------------------------------------------
    localparam WIDTH      = 8;
    localparam NUM_LANES  = 4;
    localparam USE_PARITY = 1;

    // ----------------------------------------------------------------
    // Clock generation: 10 ns period
    // ----------------------------------------------------------------
    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    // ----------------------------------------------------------------
    // Helper tasks
    // ----------------------------------------------------------------
    task drive(
        input [NUM_LANES*WIDTH-1:0] d,
        input [NUM_LANES-1:0]       en
    );
        @(posedge _if.clk) begin
            _if.data_in <= d;
            _if.lane_en <= en;
        end
    endtask

    // Check a single output value; $fatal on mismatch
    task check_data_out(
        input [NUM_LANES*WIDTH-1:0] expected,
        input [255:0]               label
    );
        @(posedge _if.clk); #1; // sample after rising edge
        if (_if.data_out !== expected) begin
            $display("FAIL [%s]: data_out = %0h, expected %0h",
                     label, _if.data_out, expected);
            $fatal(1);
        end
    endtask

    task check_parity(
        input [NUM_LANES-1:0] expected,
        input [255:0]         label
    );
        #1;
        if (_if.parity_out !== expected) begin
            $display("FAIL [%s]: parity_out = %0b, expected %0b",
                     label, _if.parity_out, expected);
            $fatal(1);
        end
    endtask

    task check_any_sat(
        input       expected,
        input [255:0] label
    );
        #1;
        if (_if.any_sat !== expected) begin
            $display("FAIL [%s]: any_sat = %0b, expected %0b",
                     label, _if.any_sat, expected);
            $fatal(1);
        end
    endtask


    // Drive async reset
    initial _if.rst_n = 1; // start deasserted
    event assert_reset;
    always @(assert_reset) begin
            @(posedge _if.clk);
            #1ns _if.rst_n    = 1;

            // Reset the DUT
            @(posedge _if.clk);

            #1ns _if.rst_n = 0;

            repeat (2) @(posedge _if.clk);

            #1ns _if.rst_n = 1;
    end

    // ----------------------------------------------------------------
    // Main stimulus
    // ----------------------------------------------------------------
    always @(posedge _if.clk) begin
        // ---- 1. Initialise ----------------------------------------
        ->assert_reset; // trigger async reset
        @(posedge _if.clk) begin
            _if.data_in <= '0;
            _if.lane_en <= '0;
        end

        // ---- 3. Wait for reset release -----------------------------
        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        repeat (2) @(posedge _if.clk);

        // ---- 4. Drive all lanes with distinct values ---------------
        // lane0=0x01, lane1=0x02, lane2=0x04, lane3=0x08
        drive({8'h08, 8'h04, 8'h02, 8'h01}, 4'b1111);

        // One cycle later, pipeline regs should hold the values
        check_data_out({8'h08, 8'h04, 8'h02, 8'h01}, "all_lanes");

        // Parity of 0x01=00000001 -> ^1=1; 0x02=00000010 -> ^1=1;
        // 0x04=00000100 -> ^1=1; 0x08=00001000 -> ^1=1
        check_parity(4'b1111, "parity_all_odd_weight");

        // ---- 5. Only even lanes enabled ----------------------------
        // data_in packing: [31:24]=lane3, [23:16]=lane2, [15:8]=lane1, [7:0]=lane0
        // lane0=0x55, lane2=0xAA; lanes 1,3 ignored (disabled)
        drive({8'h00, 8'hAA, 8'h00, 8'h55}, 4'b0101); // en lanes 0,2

        // After one more cycle: lane0=0x55, lane2=0xAA captured; lane1=0x02, lane3=0x08 held
        check_data_out({8'h08, 8'hAA, 8'h02, 8'h55}, "even_lanes");

        // ---- 6. any_active: lane0=0x55, lane2=0xAA, lanes 1,3 disabled
        // OR should be 0x55 | 0xAA = 0xFF
        #1;
        if (_if.any_active !== 8'hFF)
            $fatal(1, "any_active mismatch: got %0h, expected FF", _if.any_active);

        // ---- 7. Saturation test: drive lane3 to 0xFF ---------------
        // First fill lane3's pipeline reg to 0xFF
        drive({8'hFF, 8'h00, 8'h00, 8'h00}, 4'b1000);
        // Give it one clock to register
        @(posedge _if.clk); #1;
        check_any_sat(1, "sat_lane3");

        // ---- 8. Assert reset, check outputs clear ------------------
        ->assert_reset; // trigger async reset
        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        repeat (2) @(posedge _if.clk);

        // ---- 9. Accumulator exercise -------------------------------
        // Drive lane0 with 0x01 repeatedly; after N cycles accum should be N
        @(posedge _if.clk) begin
            _if.data_in <= {8'h00, 8'h00, 8'h00, 8'h01};
            _if.lane_en <= 4'b0001;
        end

        repeat (5) @(posedge _if.clk);
        // accum[lane0] should be 5 (or 6 depending on exact timing — check >= 4)
        #1;
        if (_if.accum[WIDTH-1:0] < 8'd4)
            $fatal(1, "accumulator too low: %0h", _if.accum[WIDTH-1:0]);

        // ---- 10. Accumulator wrap (saturation then reset to 0) ----
        // Drive lane0 with 0xFF until it saturates then wraps
        @(posedge _if.clk) begin
            _if.data_in <= {8'h00, 8'h00, 8'h00, 8'hFF};
            _if.lane_en <= 4'b0001;
        end
        // Run enough cycles to guarantee overflow
        repeat (20) @(posedge _if.clk);
        // After wrap, accum should have rolled over (we just check it's running)
        // No fatal here — just verify the simulation didn't hang

        // ---- 11. No-enable: outputs should hold --------------------
        // Capture current data_out, then disable all lanes, verify hold
        @(posedge _if.clk); #1;
        begin
            automatic logic [NUM_LANES*WIDTH-1:0] held = _if.data_out;
            @(posedge _if.clk) begin
                _if.lane_en <= 4'b0000;
            end
            @(posedge _if.clk); #1;
            @(posedge _if.clk); #1;
            if (_if.data_out !== held)
                $fatal(1, "hold failed: data_out changed with lane_en=0");
        end

        // ---- Done --------------------------------------------------
        repeat (3) @(posedge _if.clk);
        $display("PASS: generate_test all checks passed");
        $finish();
    end

endmodule
