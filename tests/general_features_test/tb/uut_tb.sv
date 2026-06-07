module uut_tb(
    uut_if.master _if
);
    logic [15:0] lfsr;
    logic [7:0] cycle;

    function automatic logic [15:0] next_lfsr(input logic [15:0] value);
        next_lfsr = {value[14:0], value[15] ^ value[13] ^ value[12] ^ value[10]};
    endfunction

    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns _if.rst_n = 1'b0;
        #26ns _if.rst_n = 1'b1;
    end

    initial begin
        lfsr = 16'hACE1;
        cycle = 8'h00;
        repeat (96) @(posedge _if.clk);
        $finish;
    end

    always @(posedge _if.clk) begin
        if (!_if.rst_n) begin
            _if.data <= 16'h0000;
            _if.idx <= 2'd0;
            _if.base <= 4'd0;
            lfsr <= 16'hACE1;
            cycle <= 8'h00;
        end else begin
            lfsr <= next_lfsr(lfsr ^ {8'hA5, cycle});
            _if.data <= lfsr ^ {8'h96, cycle};
            _if.idx <= cycle[1:0] ^ lfsr[3:2];
            _if.base <= {1'b0, cycle[2:0]} ^ {1'b0, lfsr[4:2]};
            cycle <= cycle + 8'h01;
        end
    end
endmodule
