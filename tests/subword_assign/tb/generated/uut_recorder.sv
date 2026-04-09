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
        .filepath(path("subword_in.txt")),
        .TYPE(logic [8-1:0])
    ) u_subword_in_recorder(
        .clk(_if.clk),
        .data(_if.subword_in)
    );

endmodule
