module tb_common;

    initial $timeformat(-9, -12, "ns", 10);

    initial begin
        string database_name;
        if (!$value$plusargs("WAVES=%s", database_name)) begin
            $fatal(1, "Please provide WAVES database name");
        end
        $dumpfile(database_name);
        $dumpvars(0, tb);
    end

endmodule
