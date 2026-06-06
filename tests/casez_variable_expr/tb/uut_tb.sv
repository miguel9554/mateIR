module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b0;
        #12ns _if.rst_n = 1'b1;
    end

    // Drive all inputs to known-safe values at t=0 so combinational outputs
    // are deterministic before the first clock edge.
    // sel_u=8 is safe for both unique casez outputs (code 8 hits arm1 in each).
    initial begin
        _if.sel   = 4'b0000;
        _if.sel_u = 4'b1000;
        _if.a     = 8'h00;
        _if.b     = 8'h00;
        _if.c     = 8'h00;
        _if.d     = 8'h00;
    end

    initial begin
        #300ns;
        $finish;
    end

    // sel  : full 0-15 range for non-unique casez and sequential outputs.
    // sel_u: safe range {2..6, 8..15} for unique casez outputs.
    //   Avoided codes:
    //     0,1 — uncovered in unique_nooverlap_o (no default) → X in our sim
    //     7   — conflict code in unique_overlap_o (4'b011? ∩ 4'b0111) → X in our sim
    always @(posedge _if.clk) begin
        static int step = 0;

        if (!_if.rst_n) begin
            _if.sel   <= 4'b0000;
            _if.sel_u <= 4'b0010;
            _if.a     <= 8'h00;
            _if.b     <= 8'h00;
            _if.c     <= 8'h00;
            _if.d     <= 8'h00;
            step = 0;
        end else begin
            case (step)
                // Walk all 16 sel values; sel_u cycles through {2..6, 8..15}
                0:  begin _if.sel <= 4'b0000; _if.sel_u <= 4'b0010; _if.a <= 8'hAA; _if.b <= 8'hBB; _if.c <= 8'hCC; _if.d <= 8'hDD; end
                1:  begin _if.sel <= 4'b0001; _if.sel_u <= 4'b0011; end
                2:  begin _if.sel <= 4'b0010; _if.sel_u <= 4'b0100; end
                3:  begin _if.sel <= 4'b0011; _if.sel_u <= 4'b0101; end
                4:  begin _if.sel <= 4'b0100; _if.sel_u <= 4'b0110; end
                5:  begin _if.sel <= 4'b0101; _if.sel_u <= 4'b1000; end
                6:  begin _if.sel <= 4'b0110; _if.sel_u <= 4'b1001; end
                7:  begin _if.sel <= 4'b0111; _if.sel_u <= 4'b1010; end
                8:  begin _if.sel <= 4'b1000; _if.sel_u <= 4'b1011; end
                9:  begin _if.sel <= 4'b1001; _if.sel_u <= 4'b1100; end
                10: begin _if.sel <= 4'b1010; _if.sel_u <= 4'b1101; end
                11: begin _if.sel <= 4'b1011; _if.sel_u <= 4'b1110; end
                12: begin _if.sel <= 4'b1100; _if.sel_u <= 4'b1111; end
                13: begin _if.sel <= 4'b1101; _if.sel_u <= 4'b0010; end
                14: begin _if.sel <= 4'b1110; _if.sel_u <= 4'b0011; end
                15: begin _if.sel <= 4'b1111; _if.sel_u <= 4'b0100; end
                // Second pass with different data
                16: begin _if.sel <= 4'b0000; _if.sel_u <= 4'b0110; _if.a <= 8'h11; _if.b <= 8'h22; _if.c <= 8'h33; _if.d <= 8'h44; end
                17: begin _if.sel <= 4'b1100; _if.sel_u <= 4'b1000; end
                18: begin _if.sel <= 4'b0100; _if.sel_u <= 4'b1001; end
                19: begin _if.sel <= 4'b1000; _if.sel_u <= 4'b0101; end
                20: begin _if.sel <= 4'b0001; _if.sel_u <= 4'b1111; end
                21: begin _if.sel <= 4'b0110; _if.sel_u <= 4'b0010; end
                default: $finish;
            endcase
            step = step + 1;
        end
    end
endmodule
