module uut_tb (
    uut_if.master _if
);

    initial begin
        _if.clk = $random();
        _if.subword_in = $random();
    end

    always begin
        #5 _if.clk = ~_if.clk;
    end

    initial begin
        repeat (20) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        _if.subword_in <= $random;
    end

endmodule
