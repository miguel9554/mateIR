module tb_common;
    import tb_pkg::sim_speed_start;
    import tb_pkg::sim_speed_tick;
    import tb_pkg::sim_speed_report;

    initial $timeformat(-9, -12, "ns", 10);

    initial begin
        string database_name;
        if (!$value$plusargs("WAVES=%s", database_name)) begin
            $fatal(1, "Please provide WAVES database name");
        end
        $dumpfile(database_name);
        $dumpvars(0, tb);
    end

    // Periodic simulation-speed progress. Every PROGRESS_STEP_NS of simulation
    // time we print the speed for that interval plus the cumulative average.
    // The simulation length need not be known ahead of time: this is a fixed
    // cadence, so the number of prints scales with how long the run turns out
    // to be. Override the interval with +PROGRESS_STEP_NS=<ns> (default 1us).
    initial begin
        real step_ns;
        if (!$value$plusargs("PROGRESS_STEP_NS=%f", step_ns))
            step_ns = 1000.0;  // 1us
        sim_speed_start($realtime/1s);
        forever begin
            #(step_ns);
            sim_speed_tick($realtime/1s);
        end
    end

    // Final wall-clock vs simulation-time summary for the whole run.
    final sim_speed_report($realtime/1s);

endmodule
