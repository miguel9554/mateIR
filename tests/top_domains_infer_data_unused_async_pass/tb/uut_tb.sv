module uut_tb(
    uut_if.master _if
);
    initial _if.clk = 1'b0;
    always #5 _if.clk = ~_if.clk;

    initial begin
        _if.rst_n = 1'b1;
        #1  _if.rst_n = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    initial begin
        _if.unused_in = 1'b0;
        #7  _if.unused_in = 1'b1;
        #11 _if.unused_in = 1'b0;
        #13 _if.unused_in = 1'b1;
    end

    initial begin
        repeat (20) @(posedge _if.clk);
        $finish();
    end
endmodule
