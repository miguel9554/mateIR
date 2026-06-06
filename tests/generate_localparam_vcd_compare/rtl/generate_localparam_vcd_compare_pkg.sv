package generate_localparam_vcd_compare_pkg;

typedef struct packed {
    logic       lock;
    logic [1:0] mode;
    logic       exec;
    logic       write;
    logic       read;
} cfg_t;

typedef struct packed {
    logic [2:0] perm;
    logic [4:0] tag;
} meta_t;

typedef struct packed {
    cfg_t        cfg;
    logic [1:0]  stamp;
} state_t;

endpackage
