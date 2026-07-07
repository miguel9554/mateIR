module uut_tb(
    uut_if.master _if
);
    initial _if.clk_a = 1'b0;
    always #5 _if.clk_a = ~_if.clk_a;

    // Period chosen (12, vs. clk_a's 10) so posedges never coincide: clk_a
    // edges land on odd multiples of 5, clk_b edges on even multiples of 6 —
    // a coincident edge would be a genuine cross-clock race two different
    // simulators could legitimately resolve differently.
    initial _if.clk_b = 1'b0;
    always #6 _if.clk_b = ~_if.clk_b;

    initial _if.a = 1'b0;
    always @(posedge _if.clk_a) begin
        _if.a <= ~_if.a;
    end

    initial begin
        repeat (20) @(posedge _if.clk_a);
        $finish();
    end
endmodule
