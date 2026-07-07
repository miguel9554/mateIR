module uut_tb(
    uut_if _if
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
            _if.bus.data  <= 8'h0;
            _if.bus.valid <= 1'b0;
        end else begin
            _if.bus.data  <= _if.bus.data + 8'h3;
            _if.bus.valid <= ~_if.bus.valid;
        end
    end
endmodule
