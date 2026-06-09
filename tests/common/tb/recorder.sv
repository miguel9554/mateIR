module recorder#(
    string filepath,
    type TYPE,
    bit IS_SYNC,
    int LENGTH
)(
    input logic clk,
    input TYPE data [LENGTH]
);
    integer file;
    TYPE _last [LENGTH];

    function automatic void write();
        if (!IS_SYNC) $fwrite(file, "%0t ", $realtime);
        for (int i = 0; i < LENGTH; i++) begin
            if (i > 0) $fwrite(file, ", ");
            $fwrite(file, "0x%h (%d)", data[i], data[i]);
        end
        $fwrite(file, "\n");
    endfunction

    initial begin
        file = $fopen(filepath, "w");
        if (file == 0) begin
            $display("ERROR: Cannot open file");
            $finish;
        end
        if (IS_SYNC)
            $fwrite(file, "# value at each clock\n");
        else begin
            $fwrite(file, "# time value followed by value\n");
            #0 _last = data;
            write();
        end
    end

    if (IS_SYNC) begin : g_sync
        always @(posedge clk) begin
            write();
            _last <= data;
        end
    end else begin : g_async
        always @(data) begin
            if (data !== _last) begin
                write();
                _last = data;
            end
        end
    end

    final begin
        if (IS_SYNC && _last !== data)
            write();
        $fclose(file);
    end

endmodule
