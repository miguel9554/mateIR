interface uut_if;
    // Inputs
    logic clk;
    logic a_rst;

    // Outputs
    logic [7:0] count;

    modport master(output clk, output a_rst, input count);

    modport slave(input clk, input a_rst, output count);
endinterface
