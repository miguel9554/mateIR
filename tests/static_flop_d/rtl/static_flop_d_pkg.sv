package static_flop_d_pkg;
    typedef struct packed {
        logic [3:0] tag;
        logic [3:0] data;
    } item_t;

    typedef enum logic [1:0] {
        STATE_IDLE = 2'b00,
        STATE_RUN  = 2'b01,
        STATE_DONE = 2'b10,
        STATE_ERR  = 2'b11
    } state_e;
endpackage
