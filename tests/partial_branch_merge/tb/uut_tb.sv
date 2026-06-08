module uut_tb(
    uut_if.master _if
);
    logic [7:0] cycle;

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
        cycle = 8'h00;
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.sel <= 1'b0;
            _if.base <= 8'h80;
            _if.lo <= 4'h0;
            _if.hi <= 4'hF;
            cycle <= 8'h00;
        end else begin
            _if.sel <= ~_if.sel;
            _if.base <= _if.base + 8'h13;
            _if.lo <= cycle[3:0] ^ 4'h5;
            _if.hi <= cycle[3:0] + 4'h3;
            cycle <= cycle + 8'h01;
        end
    end
endmodule
