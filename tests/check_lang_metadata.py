#!/usr/bin/env python3
"""Assert the language-metadata sidecar artifact for interface and
port-type testcases.

Compiles two interface tests with mate (a plain compile; the sidecar
artifact is written unconditionally by the frontend pipeline) and asserts
lang_metadata.json content: record kinds, attrs, and role->anchor bindings.

Usage: check_lang_metadata.py <mate-binary>
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent


def compile_and_load(mate_binary, test_name):
    rtl_dir = TESTS_DIR / test_name / "rtl"
    sources = sorted(str(p) for p in rtl_dir.glob("*.v"))
    sources += sorted(str(p) for p in rtl_dir.glob("*.sv"))
    domains = rtl_dir / f"{test_name}.domains.yaml"

    with tempfile.TemporaryDirectory(prefix=f"{test_name}_langmeta_") as tmp:
        tmp_path = Path(tmp)
        cmd = [
            str(mate_binary),
            "--top", test_name,
            "--domains", str(domains),
        ] + sources
        result = subprocess.run(cmd, cwd=tmp, capture_output=True, text=True)
        if result.returncode != 0:
            raise AssertionError(
                f"{test_name}: mate compile failed\n"
                f"stdout/stderr:\n{result.stdout}{result.stderr}"
            )
        artifact = tmp_path / "debug_output" / test_name / "lang_metadata.json"
        if not artifact.exists():
            raise AssertionError(
                f"{test_name}: lang_metadata.json was not written\n"
                f"stdout/stderr:\n{result.stdout}{result.stderr}"
            )
        return json.loads(artifact.read_text())


def index(records):
    """(kind, module_path, name) -> record"""
    out = {}
    for r in records:
        bindings = r.get("bindings", [])
        module_path = bindings[0]["anchor"]["module_path"] if bindings else ""
        out[(r["kind"], module_path, r["name"])] = r
    return out


def roles(record):
    return {b["role"]: (b["anchor"]["entity"], b["anchor"]["name"])
            for b in record["bindings"]}


def check_iface_params(records):
    by_key = index(records)

    for inst in ("b12", "b16"):
        r = by_key[("sv.interface_instance", "", inst)]
        assert r["attrs"] == {"interface": "bus_if"}, r["attrs"]
        assert roles(r) == {
            "member:gain": ("module_node", f"{inst}.gain"),
            "member:data": ("module_node", f"{inst}.data"),
            "member:valid": ("module_node", f"{inst}.valid"),
            "param:W": ("param", f"{inst}.W"),
        }, roles(r)

    for path, modport in (("u_p12", "producer"), ("u_p16", "producer"),
                          ("u_chk", "consumer")):
        r = by_key[("sv.interface_port", path, "bus")]
        assert r["attrs"] == {"interface": "bus_if", "modport": modport}, r["attrs"]
        assert roles(r) == {
            "member:gain": ("module_node", "bus.gain"),
            "member:data": ("module_node", "bus.data"),
            "member:valid": ("module_node", "bus.valid"),
            "param:W": ("param", "bus.W"),
        }, roles(r)

    assert len(records) == 5, f"expected 5 records, got {len(records)}"


def check_struct_top_ports(records):
    by_key = index(records)

    r = by_key[("sv.interface_port", "", "m_my_if")]
    assert r["attrs"] == {"interface": "my_if", "modport": "master"}, r["attrs"]
    assert roles(r) == {
        "member:my_value": ("module_node", "m_my_if.my_value"),
    }, roles(r)

    r = by_key[("sv.interface_port", "", "_my_modport_if")]
    assert r["attrs"] == {"interface": "my_modport_if", "modport": "master"}, r["attrs"]
    assert roles(r) == {
        "member:my_value": ("module_node", "_my_modport_if.my_value"),
        "param:W": ("param", "_my_modport_if.W"),
    }, roles(r)

    for port in ("in_s", "out_s"):
        r = by_key[("sv.port_type", "", port)]
        assert r["attrs"] == {"type": "struct_top_ports_pkg::payload_t"}, r["attrs"]
        assert roles(r) == {"port": ("module_node", port)}, roles(r)

    assert len(records) == 4, f"expected 4 records, got {len(records)}"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 1
    mate_binary = Path(sys.argv[1]).resolve()

    for test_name, check in (
        ("iface_params", check_iface_params),
        ("struct_top_ports", check_struct_top_ports),
    ):
        records = compile_and_load(mate_binary, test_name)
        check(records)
        print(f"lang_metadata check passed: {test_name}")

    print("Language-metadata artifact checks passed.")
    return 0


if __name__ == "__main__":
    main_result = main()
    sys.exit(main_result)
