package imported_pkg;
    typedef enum logic {
        Disabled,
        Enabled
    } state_t;
endpackage

package importing_pkg;
    import imported_pkg::*;
endpackage

module package_import_fail;
endmodule
