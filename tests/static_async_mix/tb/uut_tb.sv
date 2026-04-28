module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        _if.rst_n = 1'b0;
        _if.a = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    always #5 _if.clk = ~_if.clk;

    initial begin
        #7 _if.a = 1'b1;
        #11 _if.a = 1'b0;
        #13 _if.a = 1'b1;
        #17 _if.a = 1'b0;
    end

    initial begin
        repeat (8) @(posedge _if.clk);
        $finish();
    end
endmodule
