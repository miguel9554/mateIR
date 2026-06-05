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

    initial begin
        #200ns;
        $finish;
    end

    always @(posedge _if.clk) begin
        static int step = 0;

        if (!_if.rst_n) begin
            _if.a   <= 1'b0;
            _if.b   <= 1'b0;
            _if.c   <= 1'b0;
            _if.d   <= 1'b0;
            _if.sel <= 2'b00;
            step = 0;
        end else begin
            case (step)
                0: begin _if.a <= 1'b0; _if.b <= 1'b0; _if.c <= 1'b0; _if.d <= 1'b0; _if.sel <= 2'b00; end
                1: begin _if.a <= 1'b1; _if.b <= 1'b0; _if.c <= 1'b0; _if.d <= 1'b1; _if.sel <= 2'b01; end
                2: begin _if.a <= 1'b0; _if.b <= 1'b1; _if.c <= 1'b0; _if.d <= 1'b0; _if.sel <= 2'b10; end
                3: begin _if.a <= 1'b1; _if.b <= 1'b1; _if.c <= 1'b0; _if.d <= 1'b1; _if.sel <= 2'b11; end
                4: begin _if.a <= 1'b0; _if.b <= 1'b0; _if.c <= 1'b1; _if.d <= 1'b1; _if.sel <= 2'b00; end
                5: begin _if.a <= 1'b1; _if.b <= 1'b1; _if.c <= 1'b1; _if.d <= 1'b0; _if.sel <= 2'b10; end
                6: begin _if.a <= 1'b0; _if.b <= 1'b0; _if.c <= 1'b0; _if.d <= 1'b1; _if.sel <= 2'b11; end
                7: begin _if.a <= 1'b0; _if.b <= 1'b0; _if.c <= 1'b1; _if.d <= 1'b1; _if.sel <= 2'b01; end
                default: $finish;
            endcase
            step = step + 1;
        end
    end
endmodule
