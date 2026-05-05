module uut_tb(
    uut_if.master _if
);
    initial begin
        #1 _if.ax = 4'h2;
    end

    initial begin
        #2 _if.ay = 1'b1;
    end

    initial begin
        #3 _if.bx = 4'h7;
    end

    initial begin
        #4 _if.by = 1'b0;
    end

    initial begin
        #5 _if.sel = 1'b0;
        repeat (20) begin
            #13 _if.sel = ~_if.sel;
        end
    end

    initial begin
        #300;
        $finish;
    end
endmodule
