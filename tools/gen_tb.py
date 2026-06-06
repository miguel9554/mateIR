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
    type_str: str = ""       # e.g. "rv32m_e", "int unsigned", "" for implicit
    unpacked_dims: str = ""  # unpacked dimensions on the declarator, e.g. "[PMP_MAX_REGIONS]"


@dataclass
class Port:
    name: str
    direction: str  # "input" or "output"
    is_signed: bool
    param_dims: str  # e.g. "[WL-1:0]" or "" for scalar
    resolved_dims: str  # e.g. "[16-1:0]" with params substituted
    named_struct_type: str  # preserved named type text for struct ports
    struct_leaves: list[tuple[str, str]]  # [("field.sub", "logic [N:0]"), ...]
    unpacked_dims: str = ""  # unpacked dimensions on the declarator, e.g. "[IC_NUM_WAYS]"


@dataclass
class ModuleInfo:
    name: str
    parameters: list[Parameter]
    ports: list[Port]
    imports: list[str]  # e.g. ["import ibex_pkg::*;"]


def parse_module(filepath: Path) -> ModuleInfo:
    tree = pyslang.SyntaxTree.fromFile(str(filepath))
    root = tree.root
    module = None
    stem_name = filepath.stem
    for member in root.members:
        if 'ModuleDeclaration' not in str(member.kind):
            continue
        if not hasattr(member, 'header'):
            continue
        header = member.header
        if header.name.valueText == stem_name:
            module = member
            break
        if module is None:
            module = member
    if module is None:
        raise ValueError(f"No module declaration found in {filepath}")

    header = module.header
    mod_name = header.name.valueText

    # Parse package imports from the module header (e.g. "import ibex_pkg::*;")
    imports: list[str] = []
    try:
        for imp in header.imports:
            imp_text = str(imp).strip()
            if imp_text:
                imports.append(imp_text)
    except (AttributeError, TypeError):
        pass

    # Parse parameters
    params = []
    if header.parameters is not None:
        for decl in header.parameters.declarations:
            if not hasattr(decl, 'declarators'):
                continue  # skip comma tokens
            type_str = ""
            try:
                if 'Implicit' not in str(decl.type.kind):
                    type_str = str(decl.type).strip()
            except AttributeError:
                pass
            for d in decl.declarators:
                unpacked_dims = ""
                try:
                    parts = [str(dim).strip() for dim in d.dimensions]
                    unpacked_dims = ' '.join(parts)
                except (AttributeError, TypeError):
                    pass
                params.append(Parameter(
                    name=d.name.valueText,
                    default=str(d.initializer.expr).strip(),
                    type_str=type_str,
                    unpacked_dims=unpacked_dims,
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
    typedef_map: dict[str, tuple[str, bool, str]] = {}  # name -> (kind, is_signed, dims_str)
    struct_leaves_map: dict[str, list[tuple[str, str]]] = {}

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
                    kind = str(m.type.kind)
                    typedef_map[td_name] = (kind, is_signed, dims)
                    if 'StructType' in kind:
                        leaves: list[tuple[str, str]] = []
                        for member in m.type.members:
                            member_kind = str(member.type.kind)
                            if 'NamedType' in member_kind:
                                nested_type_name = str(member.type.name).strip()
                                nested_lookup = nested_type_name.split("::")[-1]
                                nested_leaves = struct_leaves_map.get(nested_type_name) or struct_leaves_map.get(nested_lookup) or []
                                for declarator in member.declarators:
                                    field_name = declarator.name.valueText
                                    for nested_leaf, nested_type in nested_leaves:
                                        leaves.append((f'{field_name}.{nested_leaf}', nested_type))
                            else:
                                m_signed, m_dims = _extract_base_dims(member.type)
                                type_parts = ['logic']
                                if m_signed:
                                    type_parts.append('signed')
                                if m_dims:
                                    type_parts.append(m_dims)
                                leaf_type = ' '.join(type_parts)
                                for declarator in member.declarators:
                                    leaves.append((declarator.name.valueText, leaf_type))
                        struct_leaves_map[td_name] = leaves
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
                named_struct_type = ""
                struct_leaves: list[tuple[str, str]] = []
                unqualified_name = type_name.split("::")[-1]
                lookup_name = type_name if type_name in typedef_map else unqualified_name
                if lookup_name in typedef_map:
                    td_kind, is_signed, param_dims = typedef_map[lookup_name]
                    if 'StructType' in td_kind:
                        named_struct_type = type_name
                        struct_leaves = struct_leaves_map.get(lookup_name, [])
                else:
                    # Unknown typedef — fall back to scalar logic
                    is_signed, param_dims = False, ''
                    named_struct_type = ""
                    struct_leaves = []
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
                named_struct_type = ""
                struct_leaves = []

            port_unpacked_dims = ""
            try:
                parts = [str(dim).strip() for dim in port.declarator.dimensions]
                port_unpacked_dims = ' '.join(parts)
            except (AttributeError, TypeError):
                pass

            ports.append(Port(
                name=port_name,
                direction=direction,
                is_signed=is_signed,
                param_dims=param_dims,
                resolved_dims=resolved_dims,
                named_struct_type=named_struct_type,
                struct_leaves=struct_leaves,
                unpacked_dims=port_unpacked_dims,
            ))

    return ModuleInfo(name=mod_name, parameters=params, ports=ports, imports=imports)


def port_type_str(port: Port, use_resolved: bool = False) -> str:
    """Build type string like 'logic signed [WL-1:0]' or 'logic [8-1:0]'."""
    if port.named_struct_type:
        return port.named_struct_type
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


def _normalize_signal_refs(signal_refs: list[object]) -> list[str]:
    """Extract signal names from schema-valid string forms."""
    result = []
    for signal_ref in signal_refs:
        if isinstance(signal_ref, str):
            result.append(signal_ref)
            continue
        if isinstance(signal_ref, dict):
            raise ValueError(
                "synchronized_into is no longer supported in domains YAML; "
                "use <module_name>.cdc.yaml synchronizer_flops instead"
            )
        raise ValueError(f"Expected signal reference string, got: {signal_ref!r}")
    return result


def _expand_patterns(signal_refs: list[object], port_names: set[str]) -> list[str]:
    """Expand exact names and wildcard prefixes against known port names."""
    result = []
    for pattern in _normalize_signal_refs(signal_refs):
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

    # All non-special input ports must appear in exactly one clock domain.
    domain_signals = set()
    for clk, sigs in domains.clock_domains.items():
        for sig in sigs:
            if sig in domain_signals:
                raise ValueError(f"Signal '{sig}' appears in multiple clock domains")
            domain_signals.add(sig)

    for port in module.ports:
        if port.direction != 'input':
            continue
        if port.name not in special_signals and port.name not in domain_signals:
            raise ValueError(
                f"Input port '{port.name}' is not in any clock domain, reset, or async_domain"
            )

    # All domain signals must be actual top-level input ports.
    for clk, sigs in domains.clock_domains.items():
        for sig in sigs:
            if sig not in port_names:
                raise ValueError(f"Domain signal '{sig}' is not a port of module '{module.name}'")
            if port_dirs[sig] != 'input':
                raise ValueError(f"Domain signal '{sig}' must be an input port")


def gen_uut_if(module: ModuleInfo) -> str:
    lines = []

    import_clause = ' '.join(module.imports)
    if import_clause:
        import_clause = f' {import_clause}'

    # Header
    if module.parameters:
        lines.append(f'interface uut_if{import_clause} #(')
        param_lines = []
        for p in module.parameters:
            type_part = f' {p.type_str}' if p.type_str else ''
            dim_part = f' {p.unpacked_dims}' if p.unpacked_dims else ''
            param_lines.append(f'    parameter{type_part} {p.name}{dim_part}')
        lines.append(',\n'.join(param_lines))
        lines.append(');')
    else:
        lines.append(f'interface uut_if{import_clause};')

    # Input signals
    inputs = [p for p in module.ports if p.direction == 'input']
    outputs = [p for p in module.ports if p.direction == 'output']

    lines.append('    // Inputs')
    for p in inputs:
        dim_part = f' {p.unpacked_dims}' if p.unpacked_dims else ''
        lines.append(f'    {port_type_str(p)} {p.name}{dim_part};')

    lines.append('')
    lines.append('    // Outputs')
    for p in outputs:
        dim_part = f' {p.unpacked_dims}' if p.unpacked_dims else ''
        lines.append(f'    {port_type_str(p)} {p.name}{dim_part};')

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
    import_clause = ' '.join(module.imports)
    if import_clause:
        lines.append(f'module tb {import_clause};')
    else:
        lines.append('module tb;')

    inputs = [p for p in module.ports if p.direction == 'input']
    outputs = [p for p in module.ports if p.direction == 'output']

    # Parameters section
    if has_params:
        lines.append('    // Parameters')
        for p in module.parameters:
            type_part = f' {p.type_str}' if p.type_str else ''
            dim_part = f' {p.unpacked_dims}' if p.unpacked_dims else ''
            lines.append(f'    parameter{type_part} {p.name}{dim_part} = {p.default};')
        lines.append('')

    # Signal declarations
    lines.append('    // Inputs')
    for p in inputs:
        dim_part = f' {p.unpacked_dims}' if p.unpacked_dims else ''
        lines.append(f'    {port_type_str(p)} {p.name}{dim_part};')
    lines.append('')

    lines.append('    // Outputs')
    for p in outputs:
        dim_part = f' {p.unpacked_dims}' if p.unpacked_dims else ''
        lines.append(f'    {port_type_str(p)} {p.name}{dim_part};')
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
    import_clause = (' ' + ' '.join(module.imports)) if module.imports else ''
    lines.append(f'module uut_recorder{import_clause}(')
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
    # Skip ports with unpacked array dimensions — scalar recorders can't handle them.
    async_ports = []
    for clk in all_clocks:
        if clk in input_ports and not input_ports[clk].unpacked_dims:
            async_ports.append(clk)
    for rst in all_resets:
        if rst in input_ports and not input_ports[rst].unpacked_dims:
            async_ports.append(rst)
    for sig in domains.async_domain:
        if sig in input_ports and not input_ports[sig].unpacked_dims:
            async_ports.append(sig)

    if async_ports:
        lines.append('')
        lines.append('    // Async recorders')
        for sig in async_ports:
            inst_name = f'u_{sig}_recorder'
            # Shorten reset instance name
            if sig in all_resets:
                inst_name = f'u_{sig.replace("_n", "").replace("rst", "rst")}_recorder'
            port = input_ports[sig]
            if port.struct_leaves:
                for leaf_name, leaf_type in port.struct_leaves:
                    leaf_suffix = leaf_name.replace('.', '_')
                    lines.append(f'    recorder#(')
                    lines.append(f'        .filepath(path("{sig}.{leaf_name}.txt")),')
                    lines.append(f'        .TYPE({leaf_type}),')
                    lines.append(f'        .IS_SYNC(0)')
                    lines.append(f'    ) {inst_name}_{leaf_suffix}(')
                    lines.append(f'        .clk(\'0),')
                    lines.append(f'        .data(_if.{sig}.{leaf_name})')
                    lines.append(f'    );')
            else:
                type_str = port_type_str(port, use_resolved=True)
                lines.append(f'    recorder#(')
                lines.append(f'        .filepath(path("{sig}.txt")),')
                lines.append(f'        .TYPE({type_str}),')
                lines.append(f'        .IS_SYNC(0)')
                lines.append(f'    ) {inst_name}(')
                lines.append(f'        .clk(\'0),')
                lines.append(f'        .data(_if.{sig})')
                lines.append(f'    );')

    # Sync recorders - only for input signals in clock domains
    sync_entries = []
    for clk, sigs in domains.clock_domains.items():
        for sig in sigs:
            if sig in input_ports and not input_ports[sig].unpacked_dims:
                sync_entries.append((clk, input_ports[sig]))

    if sync_entries:
        lines.append('')
        lines.append('    // Sync recorders')
        for clk, port in sync_entries:
            if port.struct_leaves:
                for leaf_name, leaf_type in port.struct_leaves:
                    leaf_suffix = leaf_name.replace('.', '_')
                    lines.append(f'    recorder#(')
                    lines.append(f'        .filepath(path("{port.name}.{leaf_name}.txt")),')
                    lines.append(f'        .TYPE({leaf_type}),')
                    lines.append(f'        .IS_SYNC(1)')
                    lines.append(f'    ) u_{port.name}_{leaf_suffix}_recorder(')
                    lines.append(f'        .clk(_if.{clk}),')
                    lines.append(f'        .data(_if.{port.name}.{leaf_name})')
                    lines.append(f'    );')
            else:
                type_str = port_type_str(port, use_resolved=True)
                lines.append(f'    recorder#(')
                lines.append(f'        .filepath(path("{port.name}.txt")),')
                lines.append(f'        .TYPE({type_str}),')
                lines.append(f'        .IS_SYNC(1)')
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

    rtl_dir = project_root / 'tests' / module_name / 'rtl'
    rtl_path = next((rtl_dir / f'{module_name}{ext}' for ext in ('.sv', '.v')
                     if (rtl_dir / f'{module_name}{ext}').exists()), None)
    domains_path = rtl_dir / f'{module_name}.domains.yaml'
    output_dir = project_root / 'tests' / module_name / 'tb' / 'generated'

    if rtl_path is None:
        raise FileNotFoundError(f"RTL file not found: {rtl_dir / module_name}(.sv|.v)")
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
