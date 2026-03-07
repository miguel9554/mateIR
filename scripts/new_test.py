#!/usr/bin/env python3
"""Create a new test structure for a given module name."""
import os
import sys
from pathlib import Path

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <module_name>")
        sys.exit(1)

    module = sys.argv[1]
    base = Path(__file__).resolve().parent.parent / "tests" / module

    if base.exists():
        print(f"Error: test '{module}' already exists at {base}")
        sys.exit(1)

    # Create directories
    (base / "rtl").mkdir(parents=True)
    (base / "tb").mkdir()
    (base / "work" / "verilator").mkdir(parents=True)

    # RTL stub
    (base / "rtl" / f"{module}.v").write_text(
f"""module {module} (
    input wire clk,
    input wire rst_n
);

endmodule
""")

    # Testbench
    (base / "tb" / f"uut_tb.sv").write_text(
f"""module uut_tb(
    uut_if.master _if
);
endmodule
""")

    # Custom-sim config
    custom_sim = base / "work" / "custom-sim"
    custom_sim.mkdir(parents=True)
    (custom_sim / "config.yaml").write_text(
f"""source_file: "tests/{module}/rtl/{module}.v"
top_module: {module}
inputs:
  clk: "tests/{module}/work/custom-sim/stimuli/clk.txt"
  rst_n: "tests/{module}/work/custom-sim/stimuli/rst_n.txt"
output_dir: "tests/{module}/work/custom-sim/output"
""")

    # TB common dir
    tb_common_path = base / "tb" / "common"
    os.symlink("../../common/tb", tb_common_path)

    # Verilator Makefile — symlink to shared template
    makefile_path = base / "work" / "verilator" / "Makefile"
    os.symlink("../../../common/verilator.mk", makefile_path)

    print(f"Created test structure at {base}")


if __name__ == "__main__":
    main()
