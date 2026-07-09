module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.a = 8'h00;
        #3 _if.a = 8'h5A;
        #7 _if.a = 8'hC3;
        #11 _if.a = 8'h3C;
        #13 _if.a = 8'hF0;
        #17 _if.a = 8'h0F;
        #19 $finish;
    end

    initial begin
        _if.b = 8'h00;
        #5 _if.b = 8'hA5;
        #7 _if.b = 8'h69;
        #11 _if.b = 8'h96;
        #13 _if.b = 8'h55;
        #17 _if.b = 8'hAA;
    end
endmodule
