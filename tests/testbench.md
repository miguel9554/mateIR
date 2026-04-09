All testbenches here follow the following driving structure which *must* be
followed:

* Each async signal is driven on its _own_ initial block. Only on these blocks
  are timing delays allowed.
* For each clock domain, _all_ of its signals are driven on their own
  always@(posedge clk) begin ... end block.
    * _no_ timing delays are allowed on these blocks.
    * Inside this block, driving must be done inside an @(posedge clk) begin ...
      end block, with a NBA assignment.
