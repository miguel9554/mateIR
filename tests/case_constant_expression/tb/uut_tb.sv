module uut_tb(
    uut_if.master _if
);
    logic [3:0] step;

    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns _if.rst_n = 1'b0;
        #12ns _if.rst_n = 1'b1;
    end

    initial begin
        step = 4'd0;
        repeat (32) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.a <= 1'b1;
            _if.b <= 1'b0;
            _if.c <= 1'b0;
            _if.d <= 1'b0;
            _if.sel <= 2'b00;
            step <= 4'd0;
        end else begin
            case (step)
                4'd0: begin _if.a <= 1'b1; _if.b <= 1'b0; _if.c <= 1'b0; _if.d <= 1'b0; _if.sel <= 2'b00; end
                4'd1: begin _if.a <= 1'b0; _if.b <= 1'b1; _if.c <= 1'b0; _if.d <= 1'b0; _if.sel <= 2'b01; end
                4'd2: begin _if.a <= 1'b1; _if.b <= 1'b0; _if.c <= 1'b0; _if.d <= 1'b1; _if.sel <= 2'b10; end
                4'd3: begin _if.a <= 1'b0; _if.b <= 1'b1; _if.c <= 1'b0; _if.d <= 1'b1; _if.sel <= 2'b11; end
                4'd4: begin _if.a <= 1'b1; _if.b <= 1'b1; _if.c <= 1'b0; _if.d <= 1'b0; _if.sel <= 2'b00; end
                4'd5: begin _if.a <= 1'b0; _if.b <= 1'b0; _if.c <= 1'b1; _if.d <= 1'b0; _if.sel <= 2'b01; end
                4'd6: begin _if.a <= 1'b0; _if.b <= 1'b1; _if.c <= 1'b0; _if.d <= 1'b1; _if.sel <= 2'b10; end
                default: begin _if.a <= ~_if.a; _if.b <= ~_if.b; _if.c <= 1'b0; _if.d <= ~_if.d; _if.sel <= _if.sel + 2'b01; end
            endcase
            step <= step + 4'd1;
        end
    end
endmodule
