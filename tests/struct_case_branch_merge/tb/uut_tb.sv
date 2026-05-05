module uut_tb(
    uut_if.master _if
);
    initial begin
        #1 _if.ax = 4'h1;
    end

    initial begin
        #2 _if.ay = 1'b0;
    end

    initial begin
        #3 _if.bx = 4'h4;
    end

    initial begin
        #4 _if.by = 1'b1;
    end

    initial begin
        #5 _if.cx = 4'h9;
    end

    initial begin
        #6 _if.cy = 1'b0;
    end

    initial begin
        #7 _if.sel = 2'b00;
        repeat (24) begin
            #11 _if.sel = _if.sel + 2'b01;
        end
    end

    initial begin
        #300;
        $finish;
    end
endmodule
