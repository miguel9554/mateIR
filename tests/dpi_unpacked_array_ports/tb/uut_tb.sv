module uut_tb(
    uut_if.master _if
);
    logic [3:0] cycle;

    initial _if.clk = 1'b0;
    always #5 _if.clk = ~_if.clk;

    initial begin
        _if.rst_n = 1'b1;
        #1 _if.rst_n = 1'b0;
        #12 _if.rst_n = 1'b1;
    end

    initial begin
        _if.data_i[0] = 8'h00;
        _if.data_i[1] = 8'h00;
        _if.flag_i[1] = 1'b0;
        _if.flag_i[0] = 1'b0;
        cycle = 4'd0;

        repeat (12) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        cycle <= cycle + 4'd1;

        _if.data_i[0] <= {4'h1, cycle};
        _if.data_i[1] <= {cycle, 4'h2};
        _if.flag_i[1] <= cycle[1];
        _if.flag_i[0] <= cycle[0];
    end
endmodule
