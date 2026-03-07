module uut_tb(
    uut_if.master _if
);
    // Clock generation
    initial begin
        _if.clk = 0;
    end

    always begin
        #5 _if.clk = ~_if.clk;
    end

    // Loopback: TX output feeds back into RX input
    always @(posedge _if.clk) begin
        _if.rxd <= _if.txd;
    end

    // Testbench procedure
    always @(posedge _if.clk) begin
        // Initialize signals
        @(posedge _if.clk) begin
            _if.rst          <= 0;
            _if.prescale     <= 16'd8;
            _if.s_axis_tdata  <= 8'd0;
            _if.s_axis_tvalid <= 0;
            _if.m_axis_tready <= 0;
            _if.rxd           <= 1;
        end

        // Assert reset
        @(posedge _if.clk) begin
            _if.rst <= 1;
        end

        repeat (4) @(posedge _if.clk);

        // Deassert reset
        @(posedge _if.clk) begin
            _if.rst <= 0;
        end

        repeat (4) @(posedge _if.clk);

        // Ready to accept RX data
        @(posedge _if.clk) begin
            _if.m_axis_tready <= 1;
        end

        // -------------------------------------------------
        // TX byte 0x55 (alternating bits)
        @(posedge _if.clk) begin
            _if.s_axis_tdata  <= 8'h55;
            _if.s_axis_tvalid <= 1;
        end

        // Wait for handshake
        do @(posedge _if.clk); while (_if.s_axis_tready != 1);

        @(posedge _if.clk) begin
            _if.s_axis_tvalid <= 0;
        end

        // Wait for TX to finish
        do @(posedge _if.clk); while (_if.tx_busy != 0);

        repeat (20) @(posedge _if.clk);

        // -------------------------------------------------
        // TX byte 0xA3
        @(posedge _if.clk) begin
            _if.s_axis_tdata  <= 8'hA3;
            _if.s_axis_tvalid <= 1;
        end

        do @(posedge _if.clk); while (_if.s_axis_tready != 1);

        @(posedge _if.clk) begin
            _if.s_axis_tvalid <= 0;
        end

        do @(posedge _if.clk); while (_if.tx_busy != 0);

        repeat (20) @(posedge _if.clk);

        // -------------------------------------------------
        // TX byte 0xFF
        @(posedge _if.clk) begin
            _if.s_axis_tdata  <= 8'hFF;
            _if.s_axis_tvalid <= 1;
        end

        do @(posedge _if.clk); while (_if.s_axis_tready != 1);

        @(posedge _if.clk) begin
            _if.s_axis_tvalid <= 0;
        end

        do @(posedge _if.clk); while (_if.tx_busy != 0);

        repeat (20) @(posedge _if.clk);

        // -------------------------------------------------
        // TX byte 0x00
        @(posedge _if.clk) begin
            _if.s_axis_tdata  <= 8'h00;
            _if.s_axis_tvalid <= 1;
        end

        do @(posedge _if.clk); while (_if.s_axis_tready != 1);

        @(posedge _if.clk) begin
            _if.s_axis_tvalid <= 0;
        end

        do @(posedge _if.clk); while (_if.tx_busy != 0);

        repeat (20) @(posedge _if.clk);

        // -------------------------------------------------
        // Back-to-back TX: 0x12 then 0x34
        @(posedge _if.clk) begin
            _if.s_axis_tdata  <= 8'h12;
            _if.s_axis_tvalid <= 1;
        end

        do @(posedge _if.clk); while (_if.s_axis_tready != 1);

        // Immediately present next byte
        @(posedge _if.clk) begin
            _if.s_axis_tdata  <= 8'h34;
            _if.s_axis_tvalid <= 1;
        end

        do @(posedge _if.clk); while (_if.s_axis_tready != 1);

        @(posedge _if.clk) begin
            _if.s_axis_tvalid <= 0;
        end

        do @(posedge _if.clk); while (_if.tx_busy != 0);

        repeat (20) @(posedge _if.clk);

        $finish;
    end

endmodule
