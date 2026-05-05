interface uut_if;
    // Inputs
    logic clk;
    logic rst_n;
    struct_top_ports_pkg::payload_t in_s;

    // Outputs
    struct_top_ports_pkg::payload_t out_s;

    modport master(output clk, output rst_n, output in_s, input out_s);

    modport slave(input clk, input rst_n, input in_s, output out_s);
endinterface
