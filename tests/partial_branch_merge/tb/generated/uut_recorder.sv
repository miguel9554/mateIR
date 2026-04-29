module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("sel.txt")),
        .TYPE(logic)
    ) u_sel_recorder(
        .data(_if.sel)
    );
    async_recorder#(
        .filepath(path("base.txt")),
        .TYPE(logic [7:0])
    ) u_base_recorder(
        .data(_if.base)
    );
    async_recorder#(
        .filepath(path("lo.txt")),
        .TYPE(logic [3:0])
    ) u_lo_recorder(
        .data(_if.lo)
    );
    async_recorder#(
        .filepath(path("hi.txt")),
        .TYPE(logic [3:0])
    ) u_hi_recorder(
        .data(_if.hi)
    );

endmodule
