module sync_recorder#(
    string filepath,
    type TYPE
)(
    input logic clk,
    input TYPE data
);
    // File handle
    integer file;
    TYPE last_data;

    // Open file at simulation start and write header
    initial begin
        file = $fopen(filepath, "w");
        if (file == 0) begin
            $display("ERROR: Cannot open file");
            $finish;
        end
        // Write header
        $fwrite(file, "# value at each clock\n");
    end

    // Whenever data changes, write to file
    always @(posedge clk) begin
        $fwrite(file, "%0d\n", data);
        last_data <= data;
    end

    // Close file at end of simulation
    final begin
        if (last_data !== data) begin
            $fwrite(file, "%0d\n", data);
        end
        $fclose(file);
    end

endmodule
