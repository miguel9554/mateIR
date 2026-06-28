package tb_pkg;
    function automatic string green(string s);
        return {"\033[32m", s, "\033[0m"};
    endfunction

    function automatic string red(string s);
        return {"\033[31m", s, "\033[0m"};
    endfunction
endpackage
