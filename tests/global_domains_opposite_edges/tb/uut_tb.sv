module uut_tb(
    uut_if.master _if
);
    initial _if.clk = 1'b0;
    always #5 _if.clk = ~_if.clk;

    initial _if.a = 1'b0;
    initial _if.b = 1'b0;

    always @(posedge _if.clk) begin
        _if.a <= ~_if.a;
        _if.b <= ~_if.b;
    end

    initial begin
        repeat (20) @(posedge _if.clk);
        $finish();
    end
endmodule
