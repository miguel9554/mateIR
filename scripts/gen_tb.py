#!/usr/bin/env python3
"""Generate testbench files (uut_if.sv, tb.sv, uut_recorder.sv) from RTL + domains.yaml."""

import re
import sys
from dataclasses import dataclass
from pathlib import Path

import pyslang
import yaml


@dataclass
class Parameter:
    name: str
    default: str


@dataclass
class Port:
    name: str
    direction: str  # "input" or "output"
    is_signed: bool
    param_dims: str  # e.g. "[WL-1:0]" or "" for scalar
    resolved_dims: str  # e.g. "[16-1:0]" with params substituted


@dataclass
class ModuleInfo:
    name: str
    parameters: list[Parameter]
    ports: list[Port]


def parse_module(filepath: Path) -> ModuleInfo:
    tree = pyslang.SyntaxTree.fromFile(str(filepath))
    root = tree.root
    module = next(
        m for m in root.members if hasattr(m, 'header')
    )
    header = module.header

    mod_name = header.name.valueText

    # Parse parameters
    params = []
    if header.parameters is not None:
        for decl in header.parameters.declarations:
            if not hasattr(decl, 'declarators'):
                continue  # skip comma tokens
            for d in decl.declarators:
                params.append(Parameter(
                    name=d.name.valueText,
                    default=str(d.initializer.expr).strip(),
                ))

    # Build param substitution map (sorted by descending name length for correct replacement)
    param_map = {p.name: p.default for p in params}
    sorted_param_names = sorted(param_map.keys(), key=len, reverse=True)

    def resolve_dims(dims_str: str) -> str:
        result = dims_str
        for name in sorted_param_names:
            result = re.sub(rf'\b{name}\b', param_map[name], result)
        return result

    # Collect typedef definitions from all *_pkg.sv files in the same RTL directory
    # so that enum-typed ports (NamedType) can be resolved to their base dimensions.
    typedef_map: dict[str, tuple[bool, str]] = {}  # name -> (is_signed, dims_str)

    def _extract_base_dims(type_node) -> tuple[bool, str]:
        """Return (is_signed, dims_str) for a DataTypeSyntax, recursing into EnumType."""
        kind_name = str(type_node.kind)
        if 'EnumType' in kind_name:
            base = type_node.baseType
            if base is not None:
                return _extract_base_dims(base)
            return False, ''   # plain `enum { ... }` — treated as 1-bit
        # ImplicitType / LogicType / RegType / IntegerType all have .signing and .dimensions
        try:
            is_signed = (type_node.signing.kind == pyslang.TokenKind.SignedKeyword)
        except AttributeError:
            is_signed = False
        try:
            dims = ' '.join(str(d).strip() for d in type_node.dimensions)
        except AttributeError:
            dims = ''
        return is_signed, dims

    def _collect_typedefs_from_members(members) -> None:
        for m in members:
            kind_name = str(m.kind)
            if 'TypedefDeclaration' in kind_name:
                try:
                    td_name = str(m.name.valueText)
                    is_signed, dims = _extract_base_dims(m.type)
                    typedef_map[td_name] = (is_signed, dims)
                except Exception:
                    pass
            # Descend into packages and modules
            if hasattr(m, 'members'):
                _collect_typedefs_from_members(m.members)

    for pkg_file in filepath.parent.glob('*_pkg.sv'):
        try:
            pkg_tree = pyslang.SyntaxTree.fromFile(str(pkg_file))
            _collect_typedefs_from_members(pkg_tree.root.members)
        except Exception:
            pass
    # Also scan the module's own file (typedefs defined at file/module scope)
    _collect_typedefs_from_members(root.members)

    # Parse ports
    ports = []
    if header.ports is not None:
        for port in header.ports.ports:
            if not hasattr(port, 'header'):
                continue  # skip comma tokens
            direction = port.header.direction.rawText
            port_name = port.declarator.name.valueText

            dt = port.header.dataType
            kind_name = str(dt.kind)

            if 'NamedType' in kind_name:
                # Port type is a user-defined typedef (e.g. an enum).
                # Resolve via the typedef_map built above.
                try:
                    type_name = str(dt.name).strip()
                except AttributeError:
                    type_name = ''
                if type_name in typedef_map:
                    is_signed, param_dims = typedef_map[type_name]
                else:
                    # Unknown typedef — fall back to scalar logic
                    is_signed, param_dims = False, ''
                resolved_dims = resolve_dims(param_dims)
            else:
                # Standard integer / implicit / logic type
                try:
                    is_signed = (dt.signing.kind == pyslang.TokenKind.SignedKeyword)
                except AttributeError:
                    is_signed = False
                dim_parts = []
                for dim in dt.dimensions:
                    dim_parts.append(str(dim).strip())
                param_dims = ' '.join(dim_parts)
                resolved_dims = resolve_dims(param_dims)

            ports.append(Port(
                name=port_name,
                direction=direction,
                is_signed=is_signed,
                param_dims=param_dims,
                resolved_dims=resolved_dims,
            ))

    return ModuleInfo(name=mod_name, parameters=params, ports=ports)


def port_type_str(port: Port, use_resolved: bool = False) -> str:
    """Build type string like 'logic signed [WL-1:0]' or 'logic [8-1:0]'."""
    parts = ['logic']
    if port.is_signed:
        parts.append('signed')
    dims = port.resolved_dims if use_resolved else port.param_dims
    if dims:
        parts.append(dims)
    return ' '.join(parts)


@dataclass
class DomainConfig:
    module_name: str
    resets: list[str]                    # actual reset port names
    clock_domains: dict[str, list[str]]  # clock_port -> [signal_names] (wildcards expanded)
    async_domain: list[str]              # async port names (wildcards expanded)


def _expand_patterns(patterns: list[str], port_names: set[str]) -> list[str]:
    """Expand a list of exact names and wildcard prefixes against known port names."""
    result = []
    for pattern in patterns:
        if pattern.endswith('*'):
            prefix = pattern[:-1]
            result.extend(n for n in port_names if n.startswith(prefix))
        else:
            result.append(pattern)
    return result


def load_domains(filepath: Path, port_names: set[str]) -> DomainConfig:
    with open(filepath) as f:
        data = yaml.safe_load(f)

    module_name = data['module_name']

    # resets: object {name: {polarity, signal_name?}} — extract actual port name
    resets = []
    for reset_name, reset_cfg in (data.get('resets') or {}).items():
        port = reset_cfg.get('signal_name', reset_name)
        resets.append(port)

    # clock_domains: object {name: {polarity, input_name?, inputs_outputs: [...]}}
    clock_domains = {}
    for domain_name, domain_cfg in (data.get('clock_domains') or {}).items():
        clock_port = domain_cfg.get('input_name', domain_name)
        raw_signals = domain_cfg.get('inputs_outputs') or []
        clock_domains[clock_port] = _expand_patterns(raw_signals, port_names)

    # async_domain: list of names/wildcards
    raw_async = data.get('async_domain') or []
    async_domain = _expand_patterns(raw_async, port_names)

    return DomainConfig(
        module_name=module_name,
        resets=resets,
        clock_domains=clock_domains,
        async_domain=async_domain,
    )


def validate(module: ModuleInfo, domains: DomainConfig):
    port_names = {p.name for p in module.ports}
    port_dirs = {p.name: p.direction for p in module.ports}
    all_clocks = set(domains.clock_domains.keys())
    all_resets = set(domains.resets)
    all_async = set(domains.async_domain)
    special_signals = all_clocks | all_resets | all_async

    # Clocks and resets must be input ports
    for sig in all_clocks | all_resets:
        if sig not in port_names:
            raise ValueError(f"Async signal '{sig}' is not a port of module '{module.name}'")
        if port_dirs[sig] != 'input':
            raise ValueError(f"Async signal '{sig}' must be an input port")

    # All non-special ports must appear in exactly one clock domain
    domain_signals = set()
    for clk, sigs in domains.clock_domains.items():
        for sig in sigs:
            if sig in domain_signals:
                raise ValueError(f"Signal '{sig}' appears in multiple clock domains")
            domain_signals.add(sig)

    for port in module.ports:
        if port.name not in special_signals and port.name not in domain_signals:
            raise ValueError(
                f"Port '{port.name}' is not in any clock domain, reset, or async_domain"
            )

    # All domain signals must be actual ports
    for clk, sigs in domains.clock_domains.items():
        for sig in sigs:
            if sig not in port_names:
                raise ValueError(f"Domain signal '{sig}' is not a port of module '{module.name}'")


def gen_uut_if(module: ModuleInfo) -> str:
    lines = []

    # Header
    if module.parameters:
        lines.append('interface uut_if#(')
        param_lines = []
        for p in module.parameters:
            param_lines.append(f'    parameter {p.name}')
        lines.append(',\n'.join(param_lines))
        lines.append(');')
    else:
        lines.append('interface uut_if;')

    # Input signals
    inputs = [p for p in module.ports if p.direction == 'input']
    outputs = [p for p in module.ports if p.direction == 'output']

    lines.append('    // Inputs')
    for p in inputs:
        lines.append(f'    {port_type_str(p)} {p.name};')

    lines.append('')
    lines.append('    // Outputs')
    for p in outputs:
        lines.append(f'    {port_type_str(p)} {p.name};')

    # Modports
    lines.append('')

    def modport_line(name: str, flip: bool) -> str:
        parts = []
        for p in module.ports:
            if flip:
                d = 'output' if p.direction == 'input' else 'input'
            else:
                d = p.direction
            parts.append(f'{d} {p.name}')
        return f'    modport {name}({", ".join(parts)});'

    lines.append(modport_line('master', flip=True))
    lines.append('')
    lines.append(modport_line('slave', flip=False))

    lines.append('endinterface')
    lines.append('')

    return '\n'.join(lines)


def gen_tb(module: ModuleInfo) -> str:
    lines = []
    has_params = len(module.parameters) > 0

    lines.append('`timescale 1ns/1ps')
    lines.append('')
    lines.append('module tb;')

    inputs = [p for p in module.ports if p.direction == 'input']
    outputs = [p for p in module.ports if p.direction == 'output']

    # Parameters section
    if has_params:
        lines.append('    // Parameters')
        for p in module.parameters:
            lines.append(f'    parameter {p.name} = {p.default};')
        lines.append('')

    # Signal declarations
    lines.append('    // Inputs')
    for p in inputs:
        lines.append(f'    {port_type_str(p)} {p.name};')
    lines.append('')

    lines.append('    // Outputs')
    for p in outputs:
        lines.append(f'    {port_type_str(p)} {p.name};')
    lines.append('')

    # Interface instantiation
    lines.append('    // Interface and connection to UUT')
    if has_params:
        lines.append('    uut_if#(')
        param_conns = []
        for p in module.parameters:
            param_conns.append(f'        .{p.name}({p.name})')
        lines.append(',\n'.join(param_conns))
        lines.append('    ) _if();')
    else:
        lines.append('    uut_if _if();')

    lines.append('')

    # Input assigns
    if has_params:
        lines.append('    // Inputs assign')
    for p in inputs:
        lines.append(f'    assign {p.name} = _if.{p.name};')

    lines.append('')

    # Output assigns
    if has_params:
        lines.append('    // Outputs assign')
    for p in outputs:
        lines.append(f'    assign _if.{p.name} = {p.name};')

    lines.append('')

    # Module instantiations
    lines.append('    // modules')
    if has_params:
        param_list = ', '.join(p.name for p in module.parameters)
        lines.append(f'    {module.name} #({param_list}) uut(.*);')
    else:
        lines.append(f'    {module.name} uut(.*);')
    lines.append('    uut_tb uut_tb(.*);')
    lines.append('    uut_recorder u_recorder(.*);')
    lines.append('    tb_common u_tb_common();')
    lines.append('')
    lines.append('endmodule')
    lines.append('')

    return '\n'.join(lines)


def gen_recorder(module: ModuleInfo, domains: DomainConfig) -> str:
    lines = []
    lines.append('module uut_recorder(')
    lines.append('    uut_if.slave _if')
    lines.append(');')
    lines.append('    localparam string base_dir = "../custom-sim/stimuli";')
    lines.append('')
    lines.append('    let path(name) = {base_dir, "/", name};')

    input_ports = {p.name: p for p in module.ports if p.direction == 'input'}
    all_clocks = list(domains.clock_domains.keys())
    all_resets = list(domains.resets)
    async_signals = set(all_clocks) | set(all_resets) | set(domains.async_domain)

    # Async recorders (clocks first, then resets, then async_domain)
    async_ports = []
    for clk in all_clocks:
        if clk in input_ports:
            async_ports.append(clk)
    for rst in all_resets:
        if rst in input_ports:
            async_ports.append(rst)
    for sig in domains.async_domain:
        if sig in input_ports:
            async_ports.append(sig)

    if async_ports:
        lines.append('')
        lines.append('    // Async recorders')
        for sig in async_ports:
            inst_name = f'u_{sig}_recorder'
            # Shorten reset instance name
            if sig in all_resets:
                inst_name = f'u_{sig.replace("_n", "").replace("rst", "rst")}_recorder'
            lines.append(f'    async_recorder#(')
            lines.append(f'        .filepath(path("{sig}.txt"))')
            lines.append(f'    ) {inst_name}(')
            lines.append(f'        .data(_if.{sig})')
            lines.append(f'    );')

    # Sync recorders - only for input signals in clock domains
    sync_entries = []
    for clk, sigs in domains.clock_domains.items():
        for sig in sigs:
            if sig in input_ports:
                sync_entries.append((clk, input_ports[sig]))

    if sync_entries:
        lines.append('')
        lines.append('    // Sync recorders')
        for clk, port in sync_entries:
            type_str = port_type_str(port, use_resolved=True)
            lines.append(f'    sync_recorder#(')
            lines.append(f'        .filepath(path("{port.name}.txt")),')
            lines.append(f'        .TYPE({type_str})')
            lines.append(f'    ) u_{port.name}_recorder(')
            lines.append(f'        .clk(_if.{clk}),')
            lines.append(f'        .data(_if.{port.name})')
            lines.append(f'    );')

    lines.append('')
    lines.append('endmodule')
    lines.append('')

    return '\n'.join(lines)


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <module_name>", file=sys.stderr)
        sys.exit(1)

    module_name = sys.argv[1]
    project_root = Path(__file__).resolve().parent.parent

    rtl_path = project_root / 'tests' / module_name / 'rtl' / f'{module_name}.v'
    domains_path = project_root / 'tests' / module_name / 'rtl' / f'{module_name}.domains.yaml'
    output_dir = project_root / 'tests' / module_name / 'tb' / 'generated'

    if not rtl_path.exists():
        raise FileNotFoundError(f"RTL file not found: {rtl_path}")
    if not domains_path.exists():
        raise FileNotFoundError(f"Domains file not found: {domains_path}")

    output_dir.mkdir(parents=True, exist_ok=True)

    module = parse_module(rtl_path)
    port_names = {p.name for p in module.ports}
    domains = load_domains(domains_path, port_names)
    validate(module, domains)

    (output_dir / 'uut_if.sv').write_text(gen_uut_if(module))
    (output_dir / 'tb.sv').write_text(gen_tb(module))
    (output_dir / 'uut_recorder.sv').write_text(gen_recorder(module, domains))

    print(f"Generated testbench files in {output_dir}")


if __name__ == '__main__':
    main()
