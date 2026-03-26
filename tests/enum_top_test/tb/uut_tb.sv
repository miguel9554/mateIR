// Testbench for enum_top_test.
// Only drives inputs — no checking.
// Goal: maximum toggling of all enum and data inputs to generate rich stimulus.
import enum_pkg::*;
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
    always @(posedge _if.clk) begin
        // Initialise all inputs
        @(posedge _if.clk) begin
            _if.cmd_in  <= CMD_NOP;
            _if.mode_in <= MODE_A;
            _if.prio_in <= PRIO_LOW;
            _if.data_a  <= 8'h00;
            _if.data_b  <= 8'h00;
        end

        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        @(posedge _if.clk);

        // ---------------------------------------------------------------
        // Phase 1: walk all cmd × prio combinations with incrementing data
        // ---------------------------------------------------------------
        for (int c = 0; c < 4; c++) begin
            for (int p = 0; p < 4; p++) begin
                @(posedge _if.clk) begin
                    _if.cmd_in  <= cmd_t'(c[1:0]);
                    _if.prio_in <= prio_t'(p[1:0]);
                    _if.data_a  <= 8'(c * 17 + p * 31 + 1);
                    _if.data_b  <= 8'(c * 13 + p * 7  + 1);
                    _if.mode_in <= mode_t'((c + p) % 3);
                end
            end
        end

        // ---------------------------------------------------------------
        // Phase 2: all mode values with fixed CMD_ADD
        // ---------------------------------------------------------------
        for (int m = 0; m < 3; m++) begin
            @(posedge _if.clk) begin
                _if.cmd_in  <= CMD_ADD;
                _if.mode_in <= mode_t'(m[1:0]);
                _if.prio_in <= PRIO_MED;
                _if.data_a  <= 8'(m * 37 + 5);
                _if.data_b  <= 8'(m * 19 + 3);
            end
        end

        // ---------------------------------------------------------------
        // Phase 3: PRIO_CRIT suppresses commands — toggle cmd while CRIT
        // ---------------------------------------------------------------
        _if.prio_in = PRIO_CRIT;
        for (int c = 0; c < 4; c++) begin
            @(posedge _if.clk) begin
                _if.cmd_in  <= cmd_t'(c[1:0]);
                _if.data_a  <= 8'hFF;
                _if.data_b  <= 8'h01;
                _if.mode_in <= mode_t'(c % 3);
            end
        end

        // ---------------------------------------------------------------
        // Phase 4: overflow / corner cases
        // ---------------------------------------------------------------
        _if.prio_in = PRIO_LOW;

        // ADD overflow: 0x7F + 0x01 = 0x80 (sign change)
        @(posedge _if.clk) begin
            _if.cmd_in <= CMD_ADD;
            _if.mode_in<= MODE_A;
            _if.data_a <= 8'h7F;
            _if.data_b <= 8'h01;
        end
        // ADD zero result
        @(posedge _if.clk) begin
            _if.cmd_in <= CMD_ADD;
            _if.mode_in<= MODE_A;
            _if.data_a <= 8'hFF;
            _if.data_b <= 8'h01;
        end
        // SUB overflow
        @(posedge _if.clk) begin
            _if.cmd_in <= CMD_SUB;
            _if.mode_in<= MODE_A;
            _if.data_a <= 8'h80;
            _if.data_b <= 8'h01;
        end
        // SUB zero
        @(posedge _if.clk) begin
            _if.cmd_in <= CMD_SUB;
            _if.mode_in<= MODE_A;
            _if.data_a <= 8'h42;
            _if.data_b <= 8'h42;
        end
        // MUL large
        @(posedge _if.clk) begin
            _if.cmd_in <= CMD_MUL;
            _if.mode_in<= MODE_A;
            _if.data_a <= 8'hFF;
            _if.data_b <= 8'hFF;
        end
        // MUL zero
        @(posedge _if.clk) begin
            _if.cmd_in <= CMD_MUL;
            _if.mode_in<= MODE_A;
            _if.data_a <= 8'h00;
            _if.data_b <= 8'h55;
        end

        // ---------------------------------------------------------------
        // Phase 5: MODE_B (invert) and MODE_C (xor) with all commands
        // ---------------------------------------------------------------
        for (int m = 1; m < 3; m++) begin
            _if.mode_in = mode_t'(m[1:0]);
            for (int c = 0; c < 4; c++) begin
                @(posedge _if.clk) begin
                    _if.cmd_in  <= cmd_t'(c[1:0]);
                    _if.prio_in <= prio_t'(c[1:0]);
                    _if.data_a  <= 8'(m * 71 + c * 13);
                    _if.data_b  <= 8'(m * 43 + c * 29);
                end
            end
        end

        // ---------------------------------------------------------------
        // Phase 6: pseudo-random sweep — exercises all enum state paths
        // ---------------------------------------------------------------
        begin
            logic [31:0] lfsr;
            lfsr = 32'hACE1_0001;
            repeat (200) begin
                @(posedge _if.clk) begin
                    lfsr <= {1'b0, lfsr[31:1]} ^ (lfsr[0] ? 32'hB400_0000 : 32'h0);
                    _if.cmd_in  <= cmd_t'(lfsr[1:0]);
                    _if.mode_in <= (lfsr[3:2] == 2'b11) ? MODE_C : mode_t'(lfsr[3:2]);
                    _if.prio_in <= prio_t'(lfsr[5:4]);
                    _if.data_a  <= lfsr[15:8];
                end
            end
        end

        // ---------------------------------------------------------------
        // Phase 7: rapid toggling — stress FSM transitions
        // ---------------------------------------------------------------
        repeat (50) begin
            @(posedge _if.clk) begin
                _if.cmd_in  <= cmd_t'(~2'(_if.cmd_in));
                _if.prio_in <= prio_t'(~2'(_if.prio_in));
                _if.data_a  <= ~_if.data_a;
                _if.data_b  <= _if.data_a + 8'h01;
            end
        end

        // Let pipeline drain
        repeat (10) @(posedge _if.clk);
        $finish;
    end

endmodule
