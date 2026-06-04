module uut_tb(
    uut_if.master _if
);
    // All synchronous driving via NBA assignments in the clk always block.
    // No checks — verification is custom-sim vs Verilator comparison.

    int cycle;

    always @(posedge _if.clk or negedge _if.rst_n) begin
        if (!_if.rst_n) begin
            cycle       <= 0;
            _if.rgb_in_i    <= 8'h00;
            _if.exc_in_i    <= 7'h00;
            _if.wide_in_i   <= 16'h0000;
            _if.smag_in_i   <= 8'h00;
            _if.nested_in_i <= 24'h000000;
            _if.flag_in_i   <= 1'b0;
        end else begin
            cycle <= cycle + 1;

            // Drive a new pattern each cycle to exercise all struct corners.
            // Patterns are chosen to hit: all-zero, all-one, MSB/LSB set,
            // mixed fields, and values that are invalid as int64 if mishandled.
            case (cycle)
                // --- rgb_t corners ---
                0: begin
                    _if.rgb_in_i <= 8'h00; // RGB_BLACK: r=0 g=0 b=0
                end
                1: begin
                    _if.rgb_in_i <= 8'hFF; // RGB_WHITE: r=7 g=7 b=3
                end
                2: begin
                    _if.rgb_in_i <= 8'hE0; // RGB_RED:   r=7 g=0 b=0
                end
                3: begin
                    _if.rgb_in_i <= 8'h1C; // RGB_GREEN: r=0 g=7 b=0
                end
                4: begin
                    _if.rgb_in_i <= 8'h03; // RGB_BLUE:  r=0 g=0 b=3
                end
                5: begin
                    _if.rgb_in_i <= 8'hA5; // r=5 g=1 b=1 (alternating bits)
                end
                6: begin
                    _if.rgb_in_i <= 8'h5A; // r=2 g=6 b=2
                end
                7: begin
                    _if.rgb_in_i <= 8'h80; // MSB set: r=4 g=0 b=0
                end

                // --- exc_cause_t corners ---
                8: begin
                    _if.exc_in_i <= 7'h00; // EXC_ALL_ZERO
                end
                9: begin
                    _if.exc_in_i <= 7'h7F; // EXC_ALL_ONE: irq_int=1 irq_ext=1 lower=31
                end
                10: begin
                    _if.exc_in_i <= 7'h43; // EXC_IRQ_SOFT: irq_ext=1 lower=3
                end
                11: begin
                    _if.exc_in_i <= 7'h47; // EXC_IRQ_TIMER: irq_ext=1 lower=7
                end
                12: begin
                    _if.exc_in_i <= 7'h01; // EXC_FAULT: lower=1
                end
                13: begin
                    _if.exc_in_i <= 7'h40; // irq_ext set, lower=0
                end
                14: begin
                    _if.exc_in_i <= 7'h1F; // lower=31 only
                end

                // --- wide_t corners ---
                15: begin
                    _if.wide_in_i <= 16'h0000; // WIDE_ZERO
                end
                16: begin
                    _if.wide_in_i <= 16'hFFFF; // WIDE_MAX
                end
                17: begin
                    _if.wide_in_i <= 16'hABCD; // WIDE_DEFAULT
                end
                18: begin
                    _if.wide_in_i <= 16'h8000; // MSB set: flags=8 addr=0
                end
                19: begin
                    _if.wide_in_i <= 16'h0001; // flags=0 addr=1
                end
                20: begin
                    _if.wide_in_i <= 16'hF000; // flags=F addr=0
                end
                21: begin
                    _if.wide_in_i <= 16'h0FFF; // flags=0 addr=FFF
                end
                22: begin
                    _if.wide_in_i <= 16'hA5A5;
                end

                // --- signed_mag_t corners ---
                23: begin
                    _if.smag_in_i <= 8'h00; // SMAG_ZERO: +0
                end
                24: begin
                    _if.smag_in_i <= 8'h2A; // SMAG_POS: +42
                end
                25: begin
                    _if.smag_in_i <= 8'hAA; // SMAG_NEG: -42
                end
                26: begin
                    _if.smag_in_i <= 8'h7F; // +127
                end
                27: begin
                    _if.smag_in_i <= 8'hFF; // -127
                end
                28: begin
                    _if.smag_in_i <= 8'h80; // -0 (sign set, mag=0)
                end

                // --- nested_t corners ---
                // nested_t = {rgb_t header[7:0], payload[15:0]}
                29: begin
                    _if.nested_in_i <= 24'h000000; // all zero
                end
                30: begin
                    _if.nested_in_i <= 24'hFFFFFF; // all ones
                end
                31: begin
                    _if.nested_in_i <= 24'hE01234; // header=RED, payload=0x1234
                end
                32: begin
                    _if.nested_in_i <= 24'hFF5A5A; // header=WHITE, payload=0x5A5A
                end
                33: begin
                    _if.nested_in_i <= 24'h03ABCD; // header=BLUE, payload=WIDE_DEFAULT.addr
                end
                34: begin
                    _if.nested_in_i <= 24'h800000; // MSB set
                end
                35: begin
                    _if.nested_in_i <= 24'hA5A5A5;
                end

                // --- flag_t corner (1-bit) ---
                36: begin
                    _if.flag_in_i <= 1'b0;
                end
                37: begin
                    _if.flag_in_i <= 1'b1;
                end

                // --- Sweep all inputs simultaneously with alternating patterns ---
                38: begin
                    _if.rgb_in_i    <= 8'h55;
                    _if.exc_in_i    <= 7'h55;
                    _if.wide_in_i   <= 16'h5555;
                    _if.smag_in_i   <= 8'h55;
                    _if.nested_in_i <= 24'h555555;
                    _if.flag_in_i   <= 1'b1;
                end
                39: begin
                    _if.rgb_in_i    <= 8'hAA;
                    _if.exc_in_i    <= 7'h2A;
                    _if.wide_in_i   <= 16'hAAAA;
                    _if.smag_in_i   <= 8'hAA;
                    _if.nested_in_i <= 24'hAAAAAA;
                    _if.flag_in_i   <= 1'b0;
                end
                default: begin
                    _if.rgb_in_i    <= 8'h00;
                    _if.exc_in_i    <= 7'h00;
                    _if.wide_in_i   <= 16'h0000;
                    _if.smag_in_i   <= 8'h00;
                    _if.nested_in_i <= 24'h000000;
                    _if.flag_in_i   <= 1'b0;
                end
            endcase
        end
    end
endmodule
