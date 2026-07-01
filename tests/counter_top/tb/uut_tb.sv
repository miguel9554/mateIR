module uut_tb(
    uut_if.master _if
);
    // initial values
    initial begin
        _if.clk = 0;
    end

    // Clock gen
    always begin
        #5 _if.clk = ~_if.clk;
    end

    // reset assign
    initial begin
        _if.a_rst = 1;
        #17 _if.a_rst = 0;
    end

    // finish sim
    initial begin
        repeat (20) @(posedge _if.clk);
        $finish();
    end

endmodule
