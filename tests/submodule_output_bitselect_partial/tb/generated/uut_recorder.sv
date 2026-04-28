module uut_recorder(
    uut_if.slave _if
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(
        .filepath(path("in_bus.txt")),
        .TYPE(logic [3:0])
    ) u_in_bus_recorder(
        .data(_if.in_bus)
    );

endmodule
