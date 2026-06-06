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
    TYPE _last;

    function automatic void write();
        if (IS_SYNC)
            $fwrite(file, "0x%h (%d)\n", data[0], data[0]);
        else
            $fwrite(file, "%0t 0x%h (%d)\n", $realtime, data[0], data[0]);
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
            #0 _last = data[0];
            write();
        end
    end

    if (IS_SYNC) begin : g_sync
        always @(posedge clk) begin
            write();
            _last <= data[0];
        end
    end else begin : g_async
        always @(data) begin
            if (data[0] !== _last) begin
                write();
                _last = data[0];
            end
        end
    end

    final begin
        if (IS_SYNC && _last !== data[0])
            write();
        $fclose(file);
    end

endmodule
