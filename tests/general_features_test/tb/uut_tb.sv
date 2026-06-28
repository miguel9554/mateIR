module uut_tb(
    uut_if.master _if
);
    logic [15:0] lfsr;
    logic [7:0] cycle;

    function automatic logic [15:0] next_lfsr(input logic [15:0] value);
        next_lfsr = {value[14:0], value[15] ^ value[13] ^ value[12] ^ value[10]};
    endfunction

    initial begin
        _if.i_clk = 1'b0;
        forever #5ns _if.i_clk = ~_if.i_clk;
    end

    initial begin
        _if.i_rst_n = 1'b1;
        #1ns _if.i_rst_n = 1'b0;
        #26ns _if.i_rst_n = 1'b1;
    end

    initial begin
        lfsr = 16'hACE1;
        cycle = 8'h00;
        repeat (96) @(posedge _if.i_clk);
        $finish;
    end

    always @(posedge _if.i_clk) begin
        if (!_if.i_rst_n) begin
            _if.i_data <= 16'h0000;
            _if.i_idx <= 2'd0;
            _if.i_base <= 4'd0;
            lfsr <= 16'hACE1;
            cycle <= 8'h00;
        end else begin
            lfsr <= next_lfsr(lfsr ^ {8'hA5, cycle});
            _if.i_data <= lfsr ^ {8'h96, cycle};
            _if.i_idx <= cycle[1:0] ^ lfsr[3:2];
            _if.i_base <= {1'b0, cycle[2:0]} ^ {1'b0, lfsr[4:2]};
            cycle <= cycle + 8'h01;
        end
    end
endmodule
