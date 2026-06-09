module uut_tb(
    uut_if.master _if
);
    initial begin
        _if.clk = 1'b0;
        forever #5ns _if.clk = ~_if.clk;
    end

    initial begin
        _if.rst_n = 1'b1;
        #1ns  _if.rst_n = 1'b0;
        #26ns _if.rst_n = 1'b1;
    end

    always @(posedge _if.clk) begin
        @(posedge _if.clk) begin
            _if.val_i    <= 8'd0;
            _if.sval_i   <= 8'sd0;
            _if.nibble_i <= 4'd0;
        end

        wait (_if.rst_n == 1'b0);
        wait (_if.rst_n == 1'b1);

        // scalar set {10, 20, 30}: hits
        @(posedge _if.clk) begin _if.val_i <= 8'd10;  _if.sval_i <= 8'sd10;  _if.nibble_i <= 4'd3;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd20;  _if.sval_i <= 8'sd20;  _if.nibble_i <= 4'd7;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd30;  _if.sval_i <= 8'sd30;  _if.nibble_i <= 4'd11; end

        // scalar set: misses
        @(posedge _if.clk) begin _if.val_i <= 8'd0;   _if.sval_i <= 8'sd0;   _if.nibble_i <= 4'd0;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd11;  _if.sval_i <= 8'sd11;  _if.nibble_i <= 4'd1;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd255; _if.sval_i <= 8'sd255; _if.nibble_i <= 4'd14; end

        // range [16:31]: boundaries
        @(posedge _if.clk) begin _if.val_i <= 8'd16;  _if.sval_i <= 8'sd16;  _if.nibble_i <= 4'd6;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd31;  _if.sval_i <= 8'sd31;  _if.nibble_i <= 4'd8;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd15;  _if.sval_i <= 8'sd15;  _if.nibble_i <= 4'd9;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd32;  _if.sval_i <= 8'sd32;  _if.nibble_i <= 4'd10; end

        // mixed {5, [16:31], 200}
        @(posedge _if.clk) begin _if.val_i <= 8'd5;   _if.sval_i <= 8'sd5;   _if.nibble_i <= 4'd15; end
        @(posedge _if.clk) begin _if.val_i <= 8'd200; _if.sval_i <= 8'sd200; _if.nibble_i <= 4'd0;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd24;  _if.sval_i <= 8'sd24;  _if.nibble_i <= 4'd4;  end

        // multi-range {[0:9], [100:109]}
        @(posedge _if.clk) begin _if.val_i <= 8'd0;   _if.sval_i <= 8'sd0;   _if.nibble_i <= 4'd5;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd9;   _if.sval_i <= 8'sd9;   _if.nibble_i <= 4'd13; end
        @(posedge _if.clk) begin _if.val_i <= 8'd100; _if.sval_i <= 8'sd100; _if.nibble_i <= 4'd2;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd109; _if.sval_i <= 8'sd109; _if.nibble_i <= 4'd12; end
        @(posedge _if.clk) begin _if.val_i <= 8'd10;  _if.sval_i <= 8'sd10;  _if.nibble_i <= 4'd0;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd110; _if.sval_i <= 8'sd110; _if.nibble_i <= 4'd0;  end

        // signed range [-64:63]
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= -8'sd64; _if.nibble_i <= 4'd0; end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <=  8'sd63; _if.nibble_i <= 4'd0; end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= -8'sd65; _if.nibble_i <= 4'd0; end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <=  8'sd64; _if.nibble_i <= 4'd0; end

        // nibble set {3, 7, 11, 15}
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd3;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd7;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd11; end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd15; end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd0;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd4;  end
        @(posedge _if.clk) begin _if.val_i <= 8'd0; _if.sval_i <= 8'sd0; _if.nibble_i <= 4'd8;  end

        // both: scalar AND nibble
        @(posedge _if.clk) begin _if.val_i <= 8'd10; _if.sval_i <= 8'sd10; _if.nibble_i <= 4'd3; end
        @(posedge _if.clk) begin _if.val_i <= 8'd20; _if.sval_i <= 8'sd20; _if.nibble_i <= 4'd7; end
        @(posedge _if.clk) begin _if.val_i <= 8'd10; _if.sval_i <= 8'sd10; _if.nibble_i <= 4'd0; end
        @(posedge _if.clk) begin _if.val_i <= 8'd11; _if.sval_i <= 8'sd11; _if.nibble_i <= 4'd3; end

        repeat (4) @(posedge _if.clk);
        $finish;
    end

endmodule
