module async_recorder#(
    string filepath
)(
    input logic data
);
    // Keep old value to detect changes
    logic old_data;

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
        old_data = data;
        $fwrite(file, "%0t %0d\n", $realtime, data);
    end

    // Whenever data changes, write to file
    always @(data) begin
        // Only write if value actually changed
        if (data !== old_data) begin
            $fwrite(file, "%0t %0d\n", $realtime, data);
            old_data = data;
        end
    end

    // Close file at end of simulation
    final begin
        $fclose(file);
    end

endmodule
