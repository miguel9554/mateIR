// ctrl_pkg.sv — control/scheduling types package
// Contains 5 typedef enums covering operations, sources, destinations,
// conditions, and schedulers.
package ctrl_pkg;

    typedef enum logic [2:0] {
        OP_NOP   = 3'b000,
        OP_LOAD  = 3'b001,
        OP_STORE = 3'b010,
        OP_ADD   = 3'b011,
        OP_SUB   = 3'b100,
        OP_AND   = 3'b101,
        OP_OR    = 3'b110,
        OP_XOR   = 3'b111
    } op_t;

    typedef enum logic [1:0] {
        SRC_REG = 2'b00,
        SRC_IMM = 2'b01,
        SRC_MEM = 2'b10,
        SRC_ALU = 2'b11
    } src_t;

    typedef enum logic [1:0] {
        DST_REG  = 2'b00,
        DST_MEM  = 2'b01,
        DST_IO   = 2'b10,
        DST_NULL = 2'b11
    } dst_t;

    typedef enum logic [1:0] {
        COND_ALWAYS = 2'b00,
        COND_ZERO   = 2'b01,
        COND_NEG    = 2'b10,
        COND_OVFL   = 2'b11
    } cond_t;

    typedef enum logic [1:0] {
        SCHED_RR   = 2'b00,
        SCHED_PRIO = 2'b01,
        SCHED_FIFO = 2'b10,
        SCHED_WFQ  = 2'b11
    } sched_t;

endpackage
