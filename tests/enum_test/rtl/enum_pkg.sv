package enum_pkg;

    typedef enum logic [1:0] {
        CMD_NOP = 2'b00,
        CMD_ADD = 2'b01,
        CMD_SUB = 2'b10,
        CMD_MUL = 2'b11
    } cmd_t;

    typedef enum logic [1:0] {
        MODE_A = 2'b00,
        MODE_B = 2'b01,
        MODE_C = 2'b10
    } mode_t;

    typedef enum logic [1:0] {
        PRIO_LOW  = 2'b00,
        PRIO_MED  = 2'b01,
        PRIO_HIGH = 2'b10,
        PRIO_CRIT = 2'b11
    } prio_t;

    typedef enum logic [2:0] {
        ST_IDLE = 3'b000,
        ST_LOAD = 3'b001,
        ST_EXEC = 3'b010,
        ST_DONE = 3'b011,
        ST_ERR  = 3'b100
    } state_t;

    typedef enum logic [1:0] {
        COLOR_RED   = 2'b00,
        COLOR_GREEN = 2'b01,
        COLOR_BLUE  = 2'b10
    } color_t;

    typedef enum logic [1:0] {
        STATUS_OK   = 2'b00,
        STATUS_OVFL = 2'b01,
        STATUS_ZERO = 2'b10,
        STATUS_ERR  = 2'b11
    } status_t;

endpackage
