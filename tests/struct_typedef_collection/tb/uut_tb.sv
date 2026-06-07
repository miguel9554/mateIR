module uut_tb(
    uut_if.master _if
);
    logic [7:0] cycle;

    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns _if.rst_n = 1'b0;
        #20ns _if.rst_n = 1'b1;
    end

    initial begin
        cycle = 8'h00;
        repeat (40) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.plain_in <= '{default: 0};
            _if.packed_in <= '{default: 0};
            _if.nested_in <= '{default: 0};
            cycle <= 8'h00;
        end else begin
            _if.plain_in.a <= cycle[0];
            _if.plain_in.b <= cycle[1];
            _if.plain_in.c <= cycle[3:0];
            _if.packed_in.data <= {cycle, ~cycle};
            _if.packed_in.nested.a <= ~cycle[0];
            _if.packed_in.nested.b <= cycle[2];
            _if.packed_in.nested.c <= cycle[3:0] + 4'h3;
            _if.nested_in.left.a <= cycle[1];
            _if.nested_in.left.b <= cycle[2];
            _if.nested_in.left.c <= cycle[3:0] ^ 4'h5;
            _if.nested_in.right.a <= cycle[3];
            _if.nested_in.right.b <= cycle[4];
            _if.nested_in.right.c <= cycle[3:0] + 4'h7;
            cycle <= cycle + 8'h01;
        end
    end
endmodule
