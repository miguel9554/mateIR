#include "mateir/resolved.h"
#include "util/debug.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>

namespace custom_hdl {

// ============================================================================
// Leaf layout helpers
// ============================================================================

size_t unpackedLeafCount(const Type& type) {
    size_t count = 1;
    for (const auto& dim : type.unpacked_dims) {
        count *= static_cast<size_t>(dim.size());
    }
    return count;
}

size_t linearUnpackedIndex(const std::vector<Dimension>& dims,
                           const std::vector<int64_t>& indices) {
    if (indices.size() != dims.size()) {
        throw CompilerError("unpacked index rank mismatch");
    }

    size_t linear = 0;
    size_t stride = 1;

    // Iterate from innermost (rightmost) dimension outward.
    for (size_t rev = dims.size(); rev-- > 0;) {
        const auto& dim = dims[rev];
        int64_t idx = indices[rev];

        int64_t lo = std::min<int64_t>(dim.left, dim.right);
        int64_t hi = std::max<int64_t>(dim.left, dim.right);
        if (idx < lo || idx > hi) {
            throw CompilerError(std::format("unpacked index {} out of bounds [{}, {}]",
                                            idx, lo, hi));
        }

        size_t pos = dim.left <= dim.right
            ? static_cast<size_t>(idx - dim.left)
            : static_cast<size_t>(dim.left - idx);

        linear += pos * stride;
        stride *= static_cast<size_t>(dim.size());
    }

    return linear;
}

std::vector<std::string> unpackedIndexSuffixes(const Type& type) {
    if (type.unpacked_dims.empty()) {
        return {""};
    }

    std::vector<std::string> result = {""};
    for (const auto& dim : type.unpacked_dims) {
        std::vector<std::string> newResult;
        int step = (dim.left <= dim.right) ? 1 : -1;
        for (const auto& prefix : result) {
            for (int i = dim.left; step > 0 ? i <= dim.right : i >= dim.right; i += step) {
                newResult.push_back(prefix + "[" + std::to_string(i) + "]");
            }
        }
        result = std::move(newResult);
    }
    return result;
}

Type dropFirstUnpackedDim(Type type) {
    if (!type.unpacked_dims.empty()) {
        type.unpacked_dims.erase(type.unpacked_dims.begin());
    }
    return type;
}

Type unpackedElementType(const Type& type) {
    return dropFirstUnpackedDim(type);
}

DFGNode* scalarLeaf(const SignalBinding& binding) {
    if (binding.leaves.size() != 1) {
        throw CompilerError(std::format(
            "scalarLeaf: expected 1 leaf, got {}", binding.leaves.size()));
    }
    return binding.leaves[0];
}

DFGNode* leafAt(const SignalBinding& binding,
                const Type& type,
                const std::vector<int64_t>& indices) {
    size_t idx = linearUnpackedIndex(type.unpacked_dims, indices);
    if (idx >= binding.leaves.size()) {
        throw CompilerError(std::format(
            "leafAt: index {} out of range (binding has {} leaves)", idx, binding.leaves.size()));
    }
    return binding.leaves[idx];
}

const std::vector<DFGNode*>& signalLeaves(const Signal& signal) {
    return signal.binding.leaves;
}

DFGNode* scalarSignalNode(const Signal& signal) {
    try {
        return scalarLeaf(signal.binding);
    } catch (const CompilerError&) {
        throw CompilerError(std::format(
            "scalarSignalNode: signal '{}' is not scalar-bound ({} leaves)",
            signal.name, signal.binding.leaves.size()));
    }
}

const std::vector<DFGNode*>& flopDLeaves(const FlopInfo& flop) {
    return flop.binding.d_leaves;
}

const std::vector<DFGNode*>& flopQLeaves(const FlopInfo& flop) {
    return flop.binding.q_leaves;
}

DFGNode* scalarFlopDNode(const FlopInfo& flop) {
    if (flop.binding.d_leaves.size() != 1) {
        throw CompilerError(std::format(
            "scalarFlopDNode: flop '{}' is not scalar-bound ({} d leaves)",
            flop.name, flop.binding.d_leaves.size()));
    }
    return flop.binding.d_leaves[0];
}

DFGNode* scalarFlopQNode(const FlopInfo& flop) {
    if (flop.binding.q_leaves.size() != 1) {
        throw CompilerError(std::format(
            "scalarFlopQNode: flop '{}' is not scalar-bound ({} q leaves)",
            flop.name, flop.binding.q_leaves.size()));
    }
    return flop.binding.q_leaves[0];
}

// ============================================================================
// Type implementation
// ============================================================================

Type Type::makeInteger(int width, bool is_signed,
                       std::vector<Dimension> packed_dims,
                       std::vector<Dimension> unpacked_dims) {
    return Type{
        .kind = TypeKind::Integer,
        .width = width,
        .metadata = IntegerInfo{.is_signed = is_signed},
        .packed_dims = std::move(packed_dims),
        .unpacked_dims = std::move(unpacked_dims)
    };
}

Type Type::makeEnum(std::string type_name, int width,
                    std::vector<EnumMember> members) {
    return Type{
        .kind = TypeKind::Enum,
        .width = width,
        .metadata = EnumInfo{
            .type_name = std::move(type_name),
            .members   = std::move(members)
        },
        .packed_dims  = {},
        .unpacked_dims = {}
    };
}

void Type::print(std::ostream& os) const {
    switch (kind) {
        case TypeKind::Integer:
            os << "Integer";
            break;
        case TypeKind::Enum:
            os << "Enum(" << enumInfo().type_name << ")";
            break;
    }
    if (!packed_dims.empty()) {
        for (const auto& dim : packed_dims) {
            os << "[" << dim.left << ":" << dim.right << "]";
        }
    } else {
        os << "[" << width << "]";
    }
    if (std::holds_alternative<IntegerInfo>(metadata)) {
        auto& intInfo = std::get<IntegerInfo>(metadata);
        os << (intInfo.is_signed ? " signed" : " unsigned");
    }
}

std::ostream& operator<<(std::ostream& os, const asyncTrigger_t& t) {
    os << (t.edge == POSEDGE ? "posedge" : "negedge")
       << ":" << t.name;
    return os;
}

void FlopInfo::print(std::ostream& os, int indent) const {
    auto indent_str = [](int n) { return std::string(n * 2, ' '); };

    os << indent_str(indent) << "Flop: " << name << std::endl;
    os << indent_str(indent + 1) << "type: ";
    type.print(os);
    os << std::endl;
    os << indent_str(indent + 1) << "flop_type: ";
    switch (flop_type) {
        case FLOP_D: os << "FLOP_D"; break;
    }
    os << std::endl;
    os << indent_str(indent + 1) << "clock: " << clock << std::endl;
    if (reset) {
        os << indent_str(indent + 1) << "reset: " << *reset << std::endl;
    }
    if (reset_value) {
        os << indent_str(indent + 1) << "reset_value: " << *reset_value << std::endl;
    }
}

void SignalBase::print(std::ostream& os) const {
    os << name << ": ";
    type.print(os);
    for (const auto& dim : type.unpacked_dims) {
        os << "[" << dim.left << ":" << dim.right << "]";
    }
}

void Module::print(int indent) const {
    auto indent_str = [](int n) { return std::string(n * 2, ' '); };

    std::cout << indent_str(indent) << "Module: " << this->name << std::endl;

    std::cout << indent_str(indent + 1) << "Parameters:" << std::endl;
    for (const auto& param : this->parameters) {
        std::cout << indent_str(indent + 2);
        param.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Inputs:" << std::endl;
    for (const auto& [name, in] : this->inputs) {
        std::cout << indent_str(indent + 2);
        in.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Outputs:" << std::endl;
    for (const auto& [name, out] : this->outputs) {
        std::cout << indent_str(indent + 2);
        out.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Signals:" << std::endl;
    for (const auto& [name, signal] : this->signals) {
        std::cout << indent_str(indent + 2);
        signal.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Flops:" << std::endl;
    for (const auto& flop : this->flops) {
        flop.print(std::cout, indent + 2);
    }

    std::cout << indent_str(indent + 1) << "Submodules:" << std::endl;
    for (const auto& sub : this->hierarchyInstantiation) {
        std::cout << indent_str(indent + 2) << sub.name;
        for (const auto& p : sub.parameters) {
            std::cout << " " << p.name << "=" << p.value;
        }
        std::cout << std::endl;
    }

    // Write DFG to files
    if (this->dfg) {
        ensureDebugOutputDir();
        std::string graphName = this->name + "_dfg";

        // Write DOT file
        std::string dotFilename = DEBUG_OUTPUT_DIR + "/" + graphName + ".dot";
        std::ofstream dotOut(dotFilename);
        if (dotOut) {
            dotOut << this->dfg->toDot(graphName);
            std::cout << indent_str(indent + 1) << "Wrote DFG to: " << dotFilename << std::endl;
        }

        // Write JSON file
        std::string jsonFilename = DEBUG_OUTPUT_DIR + "/" + graphName + ".json";
        std::ofstream jsonOut(jsonFilename);
        if (jsonOut) {
            jsonOut << this->dfg->toJson();
            std::cout << indent_str(indent + 1) << "Wrote DFG JSON to: " << jsonFilename << std::endl;
        }
    }
}

// ============================================================================
// JSON serialisation
// ============================================================================

static std::string typeToJson(const Type& t) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"kind\": \"" << (t.kind == TypeKind::Enum ? "enum" : "integer") << "\", ";
    ss << "\"width\": " << t.width << ", ";
    ss << "\"signed\": " << (t.isSigned() ? "true" : "false");
    if (!t.packed_dims.empty()) {
        ss << ", \"packed_dims\": [";
        for (size_t i = 0; i < t.packed_dims.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"left\": " << t.packed_dims[i].left
               << ", \"right\": " << t.packed_dims[i].right << "}";
        }
        ss << "]";
    }
    if (!t.unpacked_dims.empty()) {
        ss << ", \"unpacked_dims\": [";
        for (size_t i = 0; i < t.unpacked_dims.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"left\": " << t.unpacked_dims[i].left
               << ", \"right\": " << t.unpacked_dims[i].right << "}";
        }
        ss << "]";
    }
    if (t.kind == TypeKind::Enum) {
        const auto& ei = t.enumInfo();
        ss << ", \"enum_type\": \"" << ei.type_name << "\"";
        ss << ", \"enum_members\": [";
        for (size_t i = 0; i < ei.members.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"name\": \"" << ei.members[i].name
               << "\", \"value\": " << ei.members[i].value << "}";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

static std::string syncKindStr(SyncKind sk) {
    switch (sk) {
        case SyncKind::Sync:  return "sync";
        case SyncKind::Clock: return "clock";
        case SyncKind::Reset: return "reset";
        case SyncKind::Async: return "async";
    }
    return "sync";
}

static std::string signalToJson(const Signal& s) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"name\": \"" << s.name << "\", ";
    ss << "\"type\": " << typeToJson(s.type) << ", ";
    ss << "\"sync_kind\": \"" << syncKindStr(s.sync_kind) << "\"";
    if (s.clock_domain)
        ss << ", \"clock_domain\": \"" << s.clock_domain->name << "\"";
    if (s.clock_edge.has_value())
        ss << ", \"clock_edge\": \"" << (*s.clock_edge == POSEDGE ? "posedge" : "negedge") << "\"";
    ss << "}";
    return ss.str();
}

static std::string paramToJson(const Param& p) {
    std::ostringstream ss;
    ss << "{\"name\": \"" << p.name << "\", \"type\": " << typeToJson(p.type)
       << ", \"value\": " << p.value << "}";
    return ss.str();
}

static std::string flopToJson(const FlopInfo& f) {
    std::ostringstream ss;
    ss << "{";
    ss << "\"name\": \"" << f.name << "\", ";
    ss << "\"type\": " << typeToJson(f.type.type) << ", ";
    ss << "\"clock\": {\"edge\": \"" << (f.clock.edge == POSEDGE ? "posedge" : "negedge")
       << "\", \"name\": \"" << f.clock.name << "\"}";
    if (f.reset) {
        ss << ", \"reset\": {\"edge\": \"" << (f.reset->edge == POSEDGE ? "posedge" : "negedge")
           << "\", \"name\": \"" << f.reset->name << "\"}";
    }
    if (f.reset_value) {
        ss << ", \"reset_value\": " << *f.reset_value;
    }
    ss << "}";
    return ss.str();
}

static std::string moduleToJson(const Module& m, int indent) {
    auto ind = [](int n) { return std::string(n * 2, ' '); };
    std::ostringstream ss;

    ss << ind(indent) << "{\n";
    ss << ind(indent+1) << "\"name\": \"" << m.name << "\",\n";
    ss << ind(indent+1) << "\"instance_name\": \"" << m.instance_name << "\",\n";
    ss << ind(indent+1) << "\"pure_combinational\": " << (m.pure_combinational ? "true" : "false") << ",\n";

    // Parameters
    ss << ind(indent+1) << "\"parameters\": [\n";
    bool first = true;
    for (const auto& p : m.parameters) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << paramToJson(p);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Localparams
    ss << ind(indent+1) << "\"localparams\": [\n";
    first = true;
    for (const auto& p : m.localparams) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << paramToJson(p);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Inputs
    ss << ind(indent+1) << "\"inputs\": [\n";
    first = true;
    for (const auto& [name, sig] : m.inputs) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << signalToJson(sig);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Outputs
    ss << ind(indent+1) << "\"outputs\": [\n";
    first = true;
    for (const auto& [name, sig] : m.outputs) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << signalToJson(sig);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Signals
    ss << ind(indent+1) << "\"signals\": [\n";
    first = true;
    for (const auto& [name, sig] : m.signals) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << signalToJson(sig);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Flops
    ss << ind(indent+1) << "\"flops\": [\n";
    first = true;
    for (const auto& flop : m.flops) {
        if (!first) ss << ",\n";
        ss << ind(indent+2) << flopToJson(flop);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "],\n";

    // Submodules (recursive)
    ss << ind(indent+1) << "\"submodules\": [\n";
    first = true;
    for (const auto& sub : m.hierarchyInstantiation) {
        if (!first) ss << ",\n";
        ss << moduleToJson(sub, indent+2);
        first = false;
    }
    ss << "\n" << ind(indent+1) << "]\n";

    ss << ind(indent) << "}";
    return ss.str();
}

std::string Module::toJson() const {
    return moduleToJson(*this, 0) + "\n";
}

// ============================================================================
// Combinational loop detection
// ============================================================================

void validateNoCombLoops(const Module& module) {
    if (!module.dfg) return;

    const auto& nodes = module.dfg->nodes;

    // Standard Kahn's topo sort on the flat DFG.
    // INPUT and CONST nodes have in-degree 0 and are natural sources.
    // .q INPUT nodes break combinational paths (they are loop-cut points).
    std::map<const DFGNode*, int> in_degree;
    std::map<const DFGNode*, std::vector<const DFGNode*>> successors;

    for (const auto& node : nodes) {
        in_degree[node.get()] = 0;
    }

    for (const auto& node : nodes) {
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            in_degree[node.get()]++;
            successors[input.node].push_back(node.get());
        });
    }

    std::queue<const DFGNode*> q;
    for (const auto& [node, deg] : in_degree) {
        if (deg == 0) q.push(node);
    }

    std::set<const DFGNode*> sorted;
    while (!q.empty()) {
        const DFGNode* curr = q.front();
        q.pop();
        sorted.insert(curr);

        for (const DFGNode* succ : successors[curr]) {
            if (--in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    if (sorted.size() == nodes.size()) return;  // no cycles

    // Collect cycle nodes
    std::set<const DFGNode*> cycleNodes;
    for (const auto& node : nodes) {
        if (!sorted.count(node.get())) {
            cycleNodes.insert(node.get());
        }
    }

    // Write .dot file with cycle nodes highlighted
    std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
    std::filesystem::create_directories(dir);
    std::string dotPath = dir + "/combo_loop.dot";
    std::ofstream(dotPath) << module.dfg->toDot("combo_loop", cycleNodes);

    // Build informative error message
    std::string msg = std::format(
        "Combinational loop(s) detected in module '{}':\n"
        "  {} node(s) are part of a cycle.\n"
        "  DOT file written to: {}\n\n"
        "  Nodes in cycle:\n",
        module.name, cycleNodes.size(), dotPath);

    for (const DFGNode* node : cycleNodes) {
        // Show which of this node's inputs are also in the cycle
        std::string loopInputs;
        DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
            if (cycleNodes.count(input.node)) {
                if (!loopInputs.empty()) loopInputs += ", ";
                loopInputs += input.node->str();
            }
        });
        msg += std::format("    {} <- [{}]\n", node->str(), loopInputs);
    }

    throw CompilerError(msg);
}

} // namespace custom_hdl
