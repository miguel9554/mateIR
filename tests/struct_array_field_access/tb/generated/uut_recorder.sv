module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("clk.txt")),
        .TYPE(logic)
    ) u_clk_recorder(
        .data(_if.clk)
    );
    async_recorder#(
        .filepath(path("rst_n.txt")),
        .TYPE(logic)
    ) u_rst_recorder(
        .data(_if.rst_n)
    );

    // Sync recorders
    sync_recorder#(
        .filepath(path("in0.txt")),
        .TYPE(logic [3:0])
    ) u_in0_recorder(
        .clk(_if.clk),
        .data(_if.in0)
    );
    sync_recorder#(
        .filepath(path("in1.txt")),
        .TYPE(logic [3:0])
    ) u_in1_recorder(
        .clk(_if.clk),
        .data(_if.in1)
    );
    sync_recorder#(
        .filepath(path("v1.txt")),
        .TYPE(logic)
    ) u_v1_recorder(
        .clk(_if.clk),
        .data(_if.v1)
    );

endmodule
