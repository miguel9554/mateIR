module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.in = 1'b0;
        #1;
        $finish;
    end
endmodule
