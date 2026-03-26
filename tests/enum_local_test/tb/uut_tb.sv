// Testbench for enum_local_test.
// Only drives inputs — no checking.
// Goal: maximum toggling across all 4 enum dimensions (op, mode, shift, prio)
// and both data operands, to exercise every enum-driven path.
module uut_tb(
    uut_if.master _if
);

    // -----------------------------------------------------------------------
    // Clock: 10 ns period
    // -----------------------------------------------------------------------
    initial _if.clk = 0;
    always #5 _if.clk = ~_if.clk;

    initial begin
            _if.rst_n    = 1;

            // Reset the DUT
            @(posedge _if.clk);

            #1ns _if.rst_n = 0;

            repeat (4) @(posedge _if.clk);

            #1ns _if.rst_n = 1;

    end
    // -----------------------------------------------------------------------
    // Stimulus
    // -----------------------------------------------------------------------
    initial begin
        // Initialise — all-zero maps to OP_NOP / MODE_PASS / SHIFT_1 / PRIO_LOW
        _if.op_in    = 2'b00;
        _if.mode_in  = 2'b00;
        _if.shift_in = 2'b00;
        _if.prio_in  = 2'b00;
        _if.data_a   = 8'h00;
        _if.data_b   = 8'h00;

        repeat (1) @(posedge _if.clk);
        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        @(posedge _if.clk);

        // ---------------------------------------------------------------
        // Phase 1: exhaustive op × mode (4×4 = 16 combos)
        // ---------------------------------------------------------------
        for (int o = 0; o < 4; o++) begin
            for (int m = 0; m < 4; m++) begin
                @(posedge _if.clk);
                _if.op_in    = o[1:0];
                _if.mode_in  = m[1:0];
                _if.prio_in  = 2'b00;
                _if.data_a   = 8'(o * 23 + m * 17 + 5);
                _if.data_b   = 8'(o * 11 + m * 37 + 3);
                _if.shift_in = 2'(m % 4);
            end
        end

        // ---------------------------------------------------------------
        // Phase 2: all shift values with MODE_SHIFT
        // ---------------------------------------------------------------
        _if.op_in   = 2'b01;   // OP_ADD
        _if.mode_in = 2'b11;   // MODE_SHIFT
        _if.prio_in = 2'b00;
        for (int s = 0; s < 4; s++) begin
            @(posedge _if.clk);
            _if.shift_in = s[1:0];
            _if.data_a   = 8'(s * 43 + 7);
            _if.data_b   = 8'(s * 19 + 2);
        end

        // ---------------------------------------------------------------
        // Phase 3: PRIO_CRIT suppresses output — op and mode toggling
        // ---------------------------------------------------------------
        _if.prio_in = 2'b11;   // PRIO_CRIT
        for (int o = 0; o < 4; o++) begin
            @(posedge _if.clk);
            _if.op_in   = o[1:0];
            _if.mode_in = o[1:0];
            _if.data_a  = 8'hAA;
            _if.data_b  = 8'h55;
        end
        _if.prio_in = 2'b00;

        // ---------------------------------------------------------------
        // Phase 4: overflow and zero-result corners for each op
        // ---------------------------------------------------------------
        // OP_ADD overflow
        @(posedge _if.clk); _if.op_in = 2'b01; _if.mode_in = 2'b00; _if.data_a = 8'h7F; _if.data_b = 8'h01;
        // OP_ADD zero
        @(posedge _if.clk); _if.op_in = 2'b01; _if.mode_in = 2'b00; _if.data_a = 8'hFF; _if.data_b = 8'h01;
        // OP_SUB overflow
        @(posedge _if.clk); _if.op_in = 2'b10; _if.mode_in = 2'b00; _if.data_a = 8'h80; _if.data_b = 8'h01;
        // OP_SUB zero
        @(posedge _if.clk); _if.op_in = 2'b10; _if.mode_in = 2'b00; _if.data_a = 8'h42; _if.data_b = 8'h42;
        // OP_AND zero
        @(posedge _if.clk); _if.op_in = 2'b11; _if.mode_in = 2'b00; _if.data_a = 8'hAA; _if.data_b = 8'h55;
        // OP_AND non-zero
        @(posedge _if.clk); _if.op_in = 2'b11; _if.mode_in = 2'b00; _if.data_a = 8'hFF; _if.data_b = 8'hFF;
        // OP_NOP
        @(posedge _if.clk); _if.op_in = 2'b00; _if.mode_in = 2'b00; _if.data_a = 8'hDE; _if.data_b = 8'hAD;

        // ---------------------------------------------------------------
        // Phase 5: MODE_INV and MODE_XOR with arithmetic ops
        // ---------------------------------------------------------------
        for (int o = 1; o < 4; o++) begin
            // MODE_INV
            @(posedge _if.clk);
            _if.op_in   = o[1:0];
            _if.mode_in = 2'b01;
            _if.data_a  = 8'(o * 53 + 11);
            _if.data_b  = 8'(o * 29 + 7);

            // MODE_XOR
            @(posedge _if.clk);
            _if.op_in   = o[1:0];
            _if.mode_in = 2'b10;
            _if.data_a  = 8'(o * 47 + 13);
            _if.data_b  = 8'(o * 31 + 5);
        end

        // ---------------------------------------------------------------
        // Phase 6: PRIO_HIGH to trigger ACCUM state in FSM
        // ---------------------------------------------------------------
        _if.prio_in = 2'b10;   // PRIO_HIGH
        for (int o = 1; o < 4; o++) begin
            for (int m = 0; m < 4; m++) begin
                @(posedge _if.clk);
                _if.op_in   = o[1:0];
                _if.mode_in = m[1:0];
                _if.data_a  = 8'(o * 61 + m * 7);
                _if.data_b  = 8'(o * 13 + m * 41);
            end
        end
        _if.prio_in = 2'b00;

        // ---------------------------------------------------------------
        // Phase 7: pseudo-random LFSR sweep
        // ---------------------------------------------------------------
        begin
            logic [31:0] lfsr;
            lfsr = 32'hDEAD_0001;
            repeat (200) begin
                @(posedge _if.clk);
                lfsr = {1'b0, lfsr[31:1]} ^ (lfsr[0] ? 32'hB400_0000 : 32'h0);
                _if.op_in    = lfsr[1:0];
                _if.mode_in  = lfsr[3:2];
                _if.shift_in = lfsr[5:4];
                _if.prio_in  = lfsr[7:6];
                _if.data_a   = lfsr[15:8];
                _if.data_b   = lfsr[23:16];
            end
        end

        // ---------------------------------------------------------------
        // Phase 8: rapid toggling — stress all FSM arcs
        // ---------------------------------------------------------------
        repeat (60) begin
            @(posedge _if.clk);
            _if.op_in   = ~_if.op_in;
            _if.prio_in = ~_if.prio_in;
            _if.data_a  = ~_if.data_a;
            _if.data_b  = _if.data_a + 8'h01;
            _if.mode_in = _if.op_in;
        end

        // Drain
        repeat (10) @(posedge _if.clk);
        $finish;
    end

endmodule
