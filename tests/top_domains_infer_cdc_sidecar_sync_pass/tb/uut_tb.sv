module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        _if.async_in = 1'b0;
    end

    always #5 _if.clk = ~_if.clk;

    initial begin
        #3  _if.async_in = 1'b1;
        #11 _if.async_in = 1'b0;
        #17 _if.async_in = 1'b1;
        #13 _if.async_in = 1'b0;
    end

    initial begin
        repeat (20) @(posedge _if.clk);
        $finish();
    end
endmodule
