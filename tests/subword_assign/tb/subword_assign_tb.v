`timescale 1ns/1ps

module uut_recorder(
    input logic clk,
    input logic [8-1:0] subword_in,
    input logic [4-1:0] subword_out_1,
    input logic [4-1:0] subword_out_2
);
    localparam string base_dir = "../custom-sim/stimuli";

    let path(name) = {base_dir, "/", name};

    // Async recorders
    async_recorder#(.filepath(path("clk.txt"))) u_clk_recorder(.data(clk));

    // Sync recorders
    sync_recorder#(.filepath(path("subword_in.txt")), .TYPE(logic [8-1:0]))
        u_subword_in_recorder(.clk(clk), .data(subword_in));
endmodule

module tb;
    logic clk = $random;
    logic [8-1:0] subword_in = $random;

    logic [4-1:0] subword_out_1;
    logic [4-1:0] subword_out_2;
    logic [8-1:0] word_out;

    subword_assign uut(.*);
    uut_recorder u_recorder(.*);

    always begin
        #5 clk = ~clk;
    end

    always @(posedge clk) begin
        subword_in <= $random;
    end

    initial begin
        repeat (20) @(posedge clk);
        $finish;
    end

    initial $timeformat(-9, -12, "ns", 10);

    initial begin
        string database_name;
        if (!$value$plusargs("WAVES=%s", database_name)) begin
            $fatal(1, "Please provide WAVES database name");
        end
        $dumpfile(database_name);
        $dumpvars(0, tb);
    end
endmodule
