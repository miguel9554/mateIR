module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        forever #5 _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1 _if.rst_n = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    initial begin
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.seed <= 8'h0;
            _if.gain <= 4'h0;
        end else begin
            _if.seed <= _if.seed + 8'h5;
            _if.gain <= _if.gain + 4'h3;
        end
    end
endmodule
