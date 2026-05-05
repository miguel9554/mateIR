interface uut_if;
    // Inputs
    logic [1:0] sel;
    logic [3:0] ax;
    logic ay;
    logic [3:0] bx;
    logic by;
    logic [3:0] cx;
    logic cy;

    // Outputs
    logic [3:0] ox;
    logic oy;

    modport master(output sel, output ax, output ay, output bx, output by, output cx, output cy, input ox, input oy);

    modport slave(input sel, input ax, input ay, input bx, input by, input cx, input cy, output ox, output oy);
endinterface
