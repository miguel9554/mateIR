package struct_typedef_collection_pkg;
    typedef struct packed {
        logic a;
        logic b;
        logic [3:0] c;
    } plain_s_t;

    typedef struct packed {
        logic [7:0] data;
        plain_s_t nested;
    } packed_s_t;

    typedef struct packed {
        plain_s_t left;
        plain_s_t right;
    } nested_again_t;
endpackage
