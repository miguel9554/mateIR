// types_pkg.sv — geometric/physical types package
// Contains 6 typedef enums exercising varied bit-widths.
package types_pkg;

    typedef enum logic [1:0] {
        DIR_N = 2'b00,
        DIR_E = 2'b01,
        DIR_S = 2'b10,
        DIR_W = 2'b11
    } dir_t;

    typedef enum logic [2:0] {
        SHAPE_CIRCLE = 3'b000,
        SHAPE_SQUARE = 3'b001,
        SHAPE_TRI    = 3'b010,
        SHAPE_HEX    = 3'b011,
        SHAPE_STAR   = 3'b100
    } shape_t;

    typedef enum logic [1:0] {
        SIZE_TINY  = 2'b00,
        SIZE_SMALL = 2'b01,
        SIZE_MED   = 2'b10,
        SIZE_LARGE = 2'b11
    } size_t;

    typedef enum logic [1:0] {
        WEIGHT_LIGHT = 2'b00,
        WEIGHT_MED   = 2'b01,
        WEIGHT_HEAVY = 2'b10,
        WEIGHT_MAX   = 2'b11
    } weight_t;

    typedef enum logic {
        ENABLE_OFF = 1'b0,
        ENABLE_ON  = 1'b1
    } enable_t;

    typedef enum logic [2:0] {
        PHASE_INIT = 3'b000,
        PHASE_WARM = 3'b001,
        PHASE_RUN  = 3'b010,
        PHASE_COOL = 3'b011,
        PHASE_DONE = 3'b100,
        PHASE_ERR  = 3'b101
    } phase_t;

endpackage
