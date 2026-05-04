interface uut_if;
    // Inputs
    logic sel;
    logic [3:0] ax;
    logic ay;
    logic [3:0] bx;
    logic by;

    // Outputs
    logic [3:0] ox;
    logic oy;

    modport master(output sel, output ax, output ay, output bx, output by, input ox, input oy);

    modport slave(input sel, input ax, input ay, input bx, input by, output ox, output oy);
endinterface
