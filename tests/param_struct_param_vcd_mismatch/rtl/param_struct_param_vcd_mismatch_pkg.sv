package param_struct_param_vcd_mismatch_pkg;
    typedef struct packed {
        logic       lock;
        logic [1:0] mode;
        logic       exec;
        logic       write;
        logic       read;
    } cfg_t;
endpackage
