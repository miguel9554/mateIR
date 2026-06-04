package packed_struct_conv_pkg;

    // 8-bit RGB: r[2:0] g[2:0] b[1:0]
    typedef struct packed {
        logic [2:0] r;
        logic [2:0] g;
        logic [1:0] b;
    } rgb_t;

    // 7-bit exception cause (ibex-style): irq_int irq_ext lower_cause[4:0]
    typedef struct packed {
        logic       irq_int;
        logic       irq_ext;
        logic [4:0] lower_cause;
    } exc_cause_t;

    // 16-bit bus descriptor: flags[3:0] addr[11:0]
    typedef struct packed {
        logic [3:0]  flags;
        logic [11:0] addr;
    } wide_t;

    // 24-bit nested: rgb_t header + 16-bit payload
    typedef struct packed {
        rgb_t        header;
        logic [15:0] payload;
    } nested_t;

    // 1-bit single-field corner
    typedef struct packed {
        logic val;
    } flag_t;

    // 8-bit signed magnitude: sign + 7-bit magnitude
    typedef struct packed {
        logic       sign;
        logic [6:0] mag;
    } signed_mag_t;

    // Localparams: static packed struct constants
    localparam rgb_t RGB_BLACK  = '{r: 3'd0, g: 3'd0, b: 2'd0}; // 8'h00
    localparam rgb_t RGB_WHITE  = '{r: 3'd7, g: 3'd7, b: 2'd3}; // 8'hFF
    localparam rgb_t RGB_RED    = '{r: 3'd7, g: 3'd0, b: 2'd0}; // 8'hE0
    localparam rgb_t RGB_GREEN  = '{r: 3'd0, g: 3'd7, b: 2'd0}; // 8'h1C
    localparam rgb_t RGB_BLUE   = '{r: 3'd0, g: 3'd0, b: 2'd3}; // 8'h03

    localparam exc_cause_t EXC_IRQ_SOFT  = '{irq_int: 1'b0, irq_ext: 1'b1, lower_cause: 5'd3};  // 7'h43
    localparam exc_cause_t EXC_IRQ_TIMER = '{irq_int: 1'b0, irq_ext: 1'b1, lower_cause: 5'd7};  // 7'h47
    localparam exc_cause_t EXC_FAULT     = '{irq_int: 1'b0, irq_ext: 1'b0, lower_cause: 5'd1};  // 7'h01
    localparam exc_cause_t EXC_ALL_ZERO  = '{irq_int: 1'b0, irq_ext: 1'b0, lower_cause: 5'd0};  // 7'h00
    localparam exc_cause_t EXC_ALL_ONE   = '{irq_int: 1'b1, irq_ext: 1'b1, lower_cause: 5'd31}; // 7'h7F

    localparam wide_t WIDE_ZERO    = '{flags: 4'h0, addr: 12'h000}; // 16'h0000
    localparam wide_t WIDE_DEFAULT = '{flags: 4'hA, addr: 12'hBCD}; // 16'hABCD
    localparam wide_t WIDE_MAX     = '{flags: 4'hF, addr: 12'hFFF}; // 16'hFFFF

    localparam signed_mag_t SMAG_POS = '{sign: 1'b0, mag: 7'd42}; // +42
    localparam signed_mag_t SMAG_NEG = '{sign: 1'b1, mag: 7'd42}; // -42
    localparam signed_mag_t SMAG_ZERO = '{sign: 1'b0, mag: 7'd0}; // 0

    localparam nested_t NESTED_ZERO = '{header: RGB_BLACK, payload: 16'h0000};

endpackage
