interface uut_if;
    // Inputs
    logic clk;
    logic [8-1:0] subword_in;

    // Outputs
    logic [4-1:0] subword_out_1;
    logic [4-1:0] subword_out_2;
    logic [8-1:0] word_out;

    modport master(output clk, output subword_in, input subword_out_1, input subword_out_2, input word_out);

    modport slave(input clk, input subword_in, output subword_out_1, output subword_out_2, output word_out);
endinterface
