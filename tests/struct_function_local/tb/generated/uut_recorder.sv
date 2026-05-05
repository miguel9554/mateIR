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
        .filepath(path("in_a.txt")),
        .TYPE(logic [7:0])
    ) u_in_a_recorder(
        .clk(_if.clk),
        .data(_if.in_a)
    );
    sync_recorder#(
        .filepath(path("in_b.txt")),
        .TYPE(logic [7:0])
    ) u_in_b_recorder(
        .clk(_if.clk),
        .data(_if.in_b)
    );

endmodule
