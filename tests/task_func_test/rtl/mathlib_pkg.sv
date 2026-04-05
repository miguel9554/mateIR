package mathlib_pkg;
    function automatic logic [7:0] abs_diff(input logic [7:0] a, b);
        return (a >= b) ? (a - b) : (b - a);
    endfunction

    function automatic logic [7:0] clamp8(input logic [7:0] x, lo, hi);
        if      (x < lo) clamp8 = lo;
        else if (x > hi) clamp8 = hi;
        else             clamp8 = x;
    endfunction
endpackage
