interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    logic flag;
    logic [3:0] vec_a;
    logic [3:0] vec_b;

    // Outputs
    logic [3:0] plus_out;
    logic not_vec_out;
    logic not_flag_out;
    logic and_out;
    logic or_out;
    logic ne_out;
    logic nested_not_out;
    logic [3:0] case_out;

    modport master(output clk, output rst_n, output flag, output vec_a, output vec_b, input plus_out, input not_vec_out, input not_flag_out, input and_out, input or_out, input ne_out, input nested_not_out, input case_out);

    modport slave(input clk, input rst_n, input flag, input vec_a, input vec_b, output plus_out, output not_vec_out, output not_flag_out, output and_out, output or_out, output ne_out, output nested_not_out, output case_out);
endinterface
