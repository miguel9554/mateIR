module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("a.txt"))
    ) u_a_recorder(
        .data(_if.a)
    );
    async_recorder#(
        .filepath(path("b.txt"))
    ) u_b_recorder(
        .data(_if.b)
    );

endmodule
