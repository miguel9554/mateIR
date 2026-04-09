module async_recorder#(
    string filepath,
    type TYPE
)(
    input TYPE data
);
    // Keep old value to detect changes
    TYPE old_data;

    // File handle
    integer file;

    // Open file at simulation start and write header
    initial begin
        file = $fopen(filepath, "w");
        if (file == 0) begin
            $display("ERROR: Cannot open file");
            $finish;
        end
        // Write header
        $fwrite(file, "# time value followed by value\n");

        // Initialize old_data and write initial value
        #0 old_data = data;
        $fwrite(file, "%0t 0x%h (%d)\n", $realtime, data, data);
    end

    // Whenever data changes, write to file
    always @(data) begin
        // Only write if value actually changed
        if (data !== old_data) begin
            $fwrite(file, "%0t 0x%h (%d)\n", $realtime, data, data);
            old_data = data;
        end
    end

    // Close file at end of simulation
    final begin
        $fclose(file);
    end

endmodule
