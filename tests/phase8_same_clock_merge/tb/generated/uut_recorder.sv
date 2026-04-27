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

    // Sync recorders
    sync_recorder#(
        .filepath(path("a.txt")),
        .TYPE(logic)
    ) u_a_recorder(
        .clk(_if.clk),
        .data(_if.a)
    );
    sync_recorder#(
        .filepath(path("b.txt")),
        .TYPE(logic)
    ) u_b_recorder(
        .clk(_if.clk),
        .data(_if.b)
    );

endmodule
