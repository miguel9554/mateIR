module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        _if.rst_n = 1'b0;
        _if.d = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    always #5 _if.clk = ~_if.clk;

    always @(posedge _if.clk) begin
        if (_if.rst_n) _if.d <= ~_if.d;
    end

    initial begin
        repeat (8) @(posedge _if.clk);
        $finish();
    end
endmodule
