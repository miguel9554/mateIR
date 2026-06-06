module recorder#(
    string filepath,
    type TYPE,
    bit IS_SYNC
)(
    input logic clk,
    input TYPE data
);
    integer file;

    initial begin
        file = $fopen(filepath, "w");
        if (file == 0) begin
            $display("ERROR: Cannot open file");
            $finish;
        end
    end

    if (IS_SYNC) begin : g_sync
        TYPE last_data;

        initial begin
            $fwrite(file, "# value at each clock\n");
        end

        always @(posedge clk) begin
            $fwrite(file, "0x%h (%d)\n", data, data);
            last_data <= data;
        end

        final begin
            if (last_data !== data) begin
                $fwrite(file, "0x%h (%d)\n", data, data);
            end
            $fclose(file);
        end
    end else begin : g_async
        TYPE old_data;

        initial begin
            $fwrite(file, "# time value followed by value\n");
            #0 old_data = data;
            $fwrite(file, "%0t 0x%h (%d)\n", $realtime, data, data);
        end

        always @(data) begin
            if (data !== old_data) begin
                $fwrite(file, "%0t 0x%h (%d)\n", $realtime, data, data);
                old_data = data;
            end
        end

        final begin
            $fclose(file);
        end
    end

endmodule
