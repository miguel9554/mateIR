module multi_driver_child_a(
    output wire y
);
    assign y = 1'b0;
endmodule

module multi_driver_child_b(
    output wire y
);
    assign y = 1'b1;
endmodule

module multiple_submodule_output_drivers(
    output wire z
);
    wire n;

    multi_driver_child_a a(
        .y(n)
    );

    multi_driver_child_b b(
        .y(n)
    );

    assign z = n;
endmodule
