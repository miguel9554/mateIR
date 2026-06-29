module uut_tb (
    uut_if.master _if
);
    // Clock generation
    initial begin
        _if.clk = 0;
    end

    always begin
        #5 _if.clk = ~_if.clk;
    end

    // Drive async reset
    initial begin
            _if.rst_n    = 1;

            // Reset the DUT
            @(posedge _if.clk);

            #1ns _if.rst_n = 0;

            repeat (2) @(posedge _if.clk);

            #1ns _if.rst_n = 1;

    end

    int seed = 67;

    initial $urandom(seed);

    always @(posedge _if.clk) begin
        _if.angle_in <= $urandom();
    end

    // Testbench procedure
    always @(posedge _if.clk) begin
        // _if.angle_in <= $urandom(seed);
        // Initialize signals
        @(posedge _if.clk) begin
            _if.start    <= 0;
            // _if.angle_in <= 16'sd0;
        end

        // Wait for 2 clocks after reset deassertion
        wait (_if.rst_n == 0);
        wait (_if.rst_n == 1);
        repeat (2) @(posedge _if.clk);

        // -------------------------------------------------
        // Test Case 1: 0 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= 16'sd0;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 1: Angle = 0, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 2: 45 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= 16'sb0011_0010_0100_0100;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 2: Angle = 45 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 3: -45 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= -16'sb0011_0010_0100_0100;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 3: Angle = -45 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 4: 90 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= 16'sb0110_0100_1000_1000;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 4: Angle = 90 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 5: -90 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= -16'sb0110_0100_1000_1000;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 5: Angle = -90 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 6: 30 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= 16'sb0010_0001_1000_0011;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 6: Angle = 30 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 7: -60 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= 16'sb1011_1100_1111_1011;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 7: Angle = -60 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        // -------------------------------------------------
        // Test Case 8: 8.16 degrees
        @(posedge _if.clk) begin
            // _if.angle_in <= 16'sb0000_1001_0001_1101;
            _if.start    <= 1;
        end

        @(posedge _if.clk) begin
            _if.start <= 0;
        end

        do @(posedge _if.clk); while (_if.done != 1);
        $display("Test 8: Angle = 8.16 degrees, cos = %d, sin = %d", _if.cos_out, _if.sin_out);

        repeat (2) @(posedge _if.clk);
        $finish;
    end

endmodule
