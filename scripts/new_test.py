#!/usr/bin/env python3
"""Create a new test structure for a given module name."""
import os
import subprocess
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
    (base / "work" / "custom-sim" / "stimuli").mkdir(parents=True)
    (base / "work" / "vcd-diff").mkdir(parents=True)
    (base / "work" / "validate").mkdir(parents=True)

    # RTL stub
    (base / "rtl" / f"{module}.v").write_text(
f"""module {module} (
    input wire clk,
    input wire rst_n
);

endmodule
""")

    # Domains config
    (base / "rtl" / f"{module}.domains.yaml").write_text(
f"""module_name: {module}

resets:
  rst_n:
    polarity: negative

clock_domains:
  clk:
    polarity: posedge
    inputs_outputs: []
""")

    # Testbench
    (base / "tb" / f"uut_tb.sv").write_text(
f"""module uut_tb(
    uut_if.master _if
);
endmodule
""")

    # Custom-sim Makefile — symlink to shared template
    custom_sim_makefile = base / "work" / "custom-sim" / "Makefile"
    os.symlink("../../../common/custom-sim.mk", custom_sim_makefile)

    # TB common dir
    tb_common_path = base / "tb" / "common"
    os.symlink("../../common/tb", tb_common_path)

    # Verilator Makefile — symlink to shared template
    makefile_path = base / "work" / "verilator" / "Makefile"
    os.symlink("../../../common/verilator.mk", makefile_path)

    # VCD-diff Makefile — symlink to shared template
    os.symlink("../../../common/vcd-diff.mk", base / "work" / "vcd-diff" / "Makefile")

    # Validate Makefile — symlink to shared template
    os.symlink("../../../common/validate.mk", base / "work" / "validate" / "Makefile")

    # Generate TB files (uut_if.sv, tb.sv, uut_recorder.sv)
    gen_tb = Path(__file__).resolve().parent / "gen_tb.py"
    subprocess.run([sys.executable, str(gen_tb), module], check=True)

    print(f"Created test structure at {base}")


if __name__ == "__main__":
    main()
