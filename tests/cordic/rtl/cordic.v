//==============================================================================
// Module: cordic
// Description: CORDIC (COordinate Rotation DIgital Computer) algorithm
//              implementation for computing sine and cosine of input angle
// Algorithm: Rotation mode CORDIC with 15 iterations
// Format: Q1.14 fixed-point representation
// Author: Chia-Hung Hsiao
// Date: 2025/9/1
//==============================================================================
module cordic #(
    parameter WL = 16,      // Word length
    parameter FL = 14,      // Fractional length
    parameter N_ITER = 15   // Number of iterations
)(
    input  wire                clk,        // Clock signal
    input  wire                rst_n,      // Active-low reset
    input  wire                start,      // Start computation
    input  wire signed [WL-1:0] angle_in,  // Input angle in radians, Q1.14, range: [-π/2, π/2]
    output reg  signed [WL-1:0] cos_out,   // Cosine output, Q1.14
    output reg  signed [WL-1:0] test,   // Sine output, Q1.14
    output reg  signed [WL-1:0] sin_out,   // Sine output, Q1.14
    output reg                 done        // Computation complete flag
);

    //--------------------------------------------------------------------------
    // Arctan lookup table
    // Stores precomputed arctan(2^-i) values in Q1.14 format
    //--------------------------------------------------------------------------
    wire signed [WL-1:0] atan_table [0:N_ITER-1];
    wire signed [WL-1:0] atan_table_current;

    assign atan_table_current = atan_table[i];  // Current arctan value for iteration i

    always @(posedge clk) test <= angle_in+1;

    //--------------------------------------------------------------------------
    // Internal registers
    //--------------------------------------------------------------------------
    reg signed [WL-1:0] x;              // X coordinate accumulator
    reg signed [WL-1:0] y;              // Y coordinate accumulator
    reg signed [WL-1:0] z;              // Angle accumulator
    reg [4:0] i;                        // Iteration counter
    reg [1:0] state, next_state;        // FSM state registers

    //--------------------------------------------------------------------------
    // State machine definitions
    //--------------------------------------------------------------------------
    localparam IDLE   = 2'd0;
    localparam ROTATE = 2'd1;
    localparam DONE   = 2'd2;

    //--------------------------------------------------------------------------
    // CORDIC scaling factor constant
    // K ≈ 0.60725 (product of cos(arctan(2^-i)) for i=0 to infinity)
    // Represented in Q1.14 format: 0.60725 × 2^14 ≈ 9949
    //--------------------------------------------------------------------------
    localparam signed [WL-1:0] K = 16'sd9949;

    //--------------------------------------------------------------------------
    // FSM state register
    // Sequential logic for state transitions
    //--------------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            state <= IDLE;
        else
            state <= next_state;
    end

    //--------------------------------------------------------------------------
    // FSM next state logic
    // Combinational logic for determining next state
    //--------------------------------------------------------------------------
    always @(*) begin
        case (state)
            IDLE:    next_state = start ? ROTATE : IDLE;
            ROTATE:  next_state = (i == N_ITER-1) ? DONE : ROTATE;
            DONE:    next_state = IDLE;
            default: next_state = IDLE;
        endcase
    end

    // Initialize arctan lookup table (Q1.14 format)
    assign atan_table[0]  = 16'sb0011_0010_0100_0100;   // atan(2^-0)  = 45.000° ≈ 0.78539 rad
    assign atan_table[1]  = 16'sb0001_1101_1010_1100;   // atan(2^-1)  = 26.565° ≈ 0.46365 rad
    assign atan_table[2]  = 16'sb0000_1111_1010_1110;   // atan(2^-2)  = 14.036° ≈ 0.24498 rad
    assign atan_table[3]  = 16'sb0000_0111_1111_0101;   // atan(2^-3)  =  7.125° ≈ 0.12435 rad
    assign atan_table[4]  = 16'sb0000_0011_1111_1111;   // atan(2^-4)  =  3.576° ≈ 0.06241 rad
    assign atan_table[5]  = 16'sb0000_0010_0000_0000;   // atan(2^-5)  =  1.790° ≈ 0.03123 rad
    assign atan_table[6]  = 16'sb0000_0001_0000_0000;   // atan(2^-6)  =  0.895° ≈ 0.01562 rad
    assign atan_table[7]  = 16'sb0000_0000_1000_0000;   // atan(2^-7)  =  0.448° ≈ 0.00781 rad
    assign atan_table[8]  = 16'sb0000_0000_0100_0000;   // atan(2^-8)  =  0.224° ≈ 0.00391 rad
    assign atan_table[9]  = 16'sb0000_0000_0010_0000;   // atan(2^-9)  =  0.112° ≈ 0.00195 rad
    assign atan_table[10] = 16'sb0000_0000_0001_0000;   // atan(2^-10) =  0.056° ≈ 0.00098 rad
    assign atan_table[11] = 16'sb0000_0000_0000_1000;   // atan(2^-11) =  0.028° ≈ 0.00049 rad
    assign atan_table[12] = 16'sb0000_0000_0000_0100;   // atan(2^-12) =  0.014° ≈ 0.00024 rad
    assign atan_table[13] = 16'sb0000_0000_0000_0010;   // atan(2^-13) =  0.007° ≈ 0.00012 rad
    assign atan_table[14] = 16'sb0000_0000_0000_0001;   // atan(2^-14) =  0.004° ≈ 0.00006 rad

    //--------------------------------------------------------------------------
    // Main CORDIC computation
    // Implements rotation mode CORDIC algorithm with arctan LUT initialization
    //--------------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            // Reset outputs and counter
            cos_out <= 0;
            sin_out <= 0;
            i       <= 0;

        end
        else begin
/* verilator lint_off CASEINCOMPLETE */
            case (state)
                //--------------------------------------------------------------
                // IDLE state: Wait for start signal and initialize CORDIC
                //--------------------------------------------------------------
                IDLE: begin
                    if (start) begin
                        x <= K;         // Initialize x with scaling factor
                        y <= 0;         // Initialize y to zero
                        z <= angle_in;  // Load input angle
                        i <= 0;         // Reset iteration counter
                    end
                end

                //--------------------------------------------------------------
                // ROTATE state: Perform CORDIC rotation iterations
                // Rotates vector (x,y) by angle z using micro-rotations
                //--------------------------------------------------------------
                ROTATE: begin
                    if (z[WL-1] == 0) begin
                        // Positive angle: rotate counter-clockwise
                        x <= x - (y >>> i);
                        y <= y + (x >>> i);
                        z <= z - atan_table_current;
                    end
                    else begin
                        // Negative angle: rotate clockwise
                        x <= x + (y >>> i);
                        y <= y - (x >>> i);
                        z <= z + atan_table_current;
                    end
                    i <= i + 1;  // Increment iteration counter
                end

                //--------------------------------------------------------------
                // DONE state: Latch final results
                //--------------------------------------------------------------
                DONE: begin
                    cos_out <= x;  // Final x is cosine
                    sin_out <= y;  // Final y is sine
                end
            endcase
        end
    end

    //--------------------------------------------------------------------------
    // Done flag generation
    // Indicates when computation is complete
    //--------------------------------------------------------------------------
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n)
            done <= 0;
        else begin
            case (state)
                IDLE:    done <= 0;
                DONE:    done <= 1;
                default: done <= done;
            endcase
        end
    end

endmodule
