package body_import_header_leak_pkg;
    parameter int WIDTH = 4;
endpackage

module body_import_header_leak_fail (
    input logic [WIDTH-1:0] in
);
    import body_import_header_leak_pkg::*;
endmodule
