module uut_tb(
    uut_if.master _if
);
initial begin
    #10ns;
    $finish();
end
endmodule
