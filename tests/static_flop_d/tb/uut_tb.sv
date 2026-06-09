module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        _if.rst_n = 1'b1;
        #1 _if.rst_n = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    always #5 _if.clk = ~_if.clk;

    initial begin
        repeat (8) @(posedge _if.clk);
        $finish();
    end
endmodule
