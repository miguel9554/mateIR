package tb_pkg;
    function automatic logic [31:0] next_lfsr(input logic [31:0] value);
        return {value[30:0], value[31] ^ value[21] ^ value[1] ^ value[0]};
    endfunction

    function automatic string green(string s);
        return {"\033[32m", s, "\033[0m"};
    endfunction

    function automatic string red(string s);
        return {"\033[31m", s, "\033[0m"};
    endfunction
endpackage
