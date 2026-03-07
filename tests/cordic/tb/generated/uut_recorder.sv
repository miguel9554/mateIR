module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("clk.txt"))
    ) u_clk_recorder(
        .data(_if.clk)
    );
    async_recorder#(
        .filepath(path("rst_n.txt"))
    ) u_rst_recorder(
        .data(_if.rst_n)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("start.txt")),
        .TYPE(logic)
    ) u_start_recorder(
        .clk(_if.clk),
        .data(_if.start)
    );
    sync_recorder#(
        .filepath(path("angle_in.txt")),
        .TYPE(logic signed [16-1:0])
    ) u_angle_in_recorder(
        .clk(_if.clk),
        .data(_if.angle_in)
    );

endmodule
