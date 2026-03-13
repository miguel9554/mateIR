#include "ir/resolved.h"
#include "util/debug.h"

#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <set>

namespace custom_hdl {

// ============================================================================
// ResolvedType implementation
// ============================================================================

ResolvedType ResolvedType::makeInteger(int width, bool is_signed,
                                       std::vector<ResolvedDimension> packed_dims,
                                       std::vector<ResolvedDimension> unpacked_dims) {
    return ResolvedType{
        .kind = ResolvedTypeKind::Integer,
        .width = width,
        .metadata = ResolvedIntegerInfo{.is_signed = is_signed},
        .packed_dims = std::move(packed_dims),
        .unpacked_dims = std::move(unpacked_dims)
    };
}

void ResolvedType::print(std::ostream& os) const {
    switch (kind) {
        case ResolvedTypeKind::Clock:
            os << "Clock";
            break;
        case ResolvedTypeKind::Reset:
            os << "Reset";
            break;
        case ResolvedTypeKind::Integer:
            os << "Integer";
            break;
    }
    if (!packed_dims.empty()) {
        for (const auto& dim : packed_dims) {
            os << "[" << dim.left << ":" << dim.right << "]";
        }
    } else {
        os << "[" << width << "]";
    }
    if (std::holds_alternative<ResolvedIntegerInfo>(metadata)) {
        auto& intInfo = std::get<ResolvedIntegerInfo>(metadata);
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

void ResolvedSignalBase::print(std::ostream& os) const {
    os << name << ": ";
    type.print(os);
    for (const auto& dim : type.unpacked_dims) {
        os << "[" << dim.left << ":" << dim.right << "]";
    }
}

void ResolvedModule::print(int indent) const {
    auto indent_str = [](int n) { return std::string(n * 2, ' '); };

    std::cout << indent_str(indent) << "Module: " << this->name << std::endl;

    std::cout << indent_str(indent + 1) << "Parameters:" << std::endl;
    for (const auto& param : this->parameters) {
        std::cout << indent_str(indent + 2);
        param.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Inputs:" << std::endl;
    for (const auto& in : this->inputs) {
        std::cout << indent_str(indent + 2);
        in.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Outputs:" << std::endl;
    for (const auto& out : this->outputs) {
        std::cout << indent_str(indent + 2);
        out.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Signals:" << std::endl;
    for (const auto& signal : this->signals) {
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
// Combinational loop detection
// ============================================================================

void validateNoCombLoops(const ResolvedModule& module) {
    if (!module.dfg) return;

    const auto& nodes = module.dfg->nodes;

    // Virtual node: {DFGNode*, port}.
    // Non-MODULE nodes get one virtual node {node, -1}.
    // MODULE nodes get one virtual node per output port: {node, 0}, {node, 1}, ...
    using VNode = std::pair<const DFGNode*, int>;

    std::map<VNode, int> in_degree;
    std::map<VNode, std::vector<VNode>> successors;

    // Helper: find combo_deps for a module type via hierarchyInstantiation
    auto findSubComboDeps = [&](const std::string& typeName) -> const ComboDeps* {
        for (const auto& sub : module.hierarchyInstantiation) {
            if (sub.name == typeName) return &sub.combo_deps;
        }
        return nullptr;
    };

    // 1. Create all virtual nodes
    for (const auto& node : nodes) {
        if (node->op == DFGOp::MODULE) {
            int nOutputs = node->num_outputs();
            for (int p = 0; p < nOutputs; p++) {
                in_degree[{node.get(), p}] = 0;
            }
        } else {
            in_degree[{node.get(), -1}] = 0;
        }
    }

    // 2. Build edges
    for (const auto& node : nodes) {
        if (node->op == DFGOp::MODULE) continue; // MODULE edges handled below

        VNode dst = {node.get(), -1};

        for (const auto& input : node->in) {
            VNode src;
            if (input.node->op == DFGOp::MODULE) {
                src = {input.node, input.port};
            } else {
                src = {input.node, -1};
            }
            in_degree[dst]++;
            successors[src].push_back(dst);
        }
    }

    // MODULE internal edges: for each output vnode, add edges from input drivers
    for (const auto& node : nodes) {
        if (node->op != DFGOp::MODULE) continue;

        const std::string& moduleType = std::get<std::string>(node->data);
        const ComboDeps* deps = findSubComboDeps(moduleType);

        int nOutputs = node->num_outputs();
        for (int p = 0; p < nOutputs; p++) {
            VNode dst = {node.get(), p};

            if (deps && !deps->empty() &&
                p < static_cast<int>(node->output_names.size())) {
                // Use combo_deps: only edges from input ports this output depends on
                const std::string& outPortName = node->output_names[p];
                auto it = deps->find(outPortName);
                if (it != deps->end()) {
                    for (const auto& inputPort : it->second) {
                        int idx = node->input_index(inputPort);
                        if (idx >= 0 && idx < static_cast<int>(node->in.size())) {
                            VNode src;
                            if (node->in[idx].node->op == DFGOp::MODULE) {
                                src = {node->in[idx].node, node->in[idx].port};
                            } else {
                                src = {node->in[idx].node, -1};
                            }
                            in_degree[dst]++;
                            successors[src].push_back(dst);
                        }
                    }
                }
                // If output not in combo_deps, it has no combinational deps — no edges
            } else {
                // Conservative fallback: all inputs feed all outputs
                for (const auto& input : node->in) {
                    VNode src;
                    if (input.node->op == DFGOp::MODULE) {
                        src = {input.node, input.port};
                    } else {
                        src = {input.node, -1};
                    }
                    in_degree[dst]++;
                    successors[src].push_back(dst);
                }
            }
        }
    }

    // 3. Kahn's algorithm
    std::queue<VNode> q;
    for (const auto& [vn, deg] : in_degree) {
        if (deg == 0) q.push(vn);
    }

    std::set<VNode> sorted;
    while (!q.empty()) {
        VNode curr = q.front();
        q.pop();
        sorted.insert(curr);

        for (const VNode& succ : successors[curr]) {
            if (--in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    size_t totalVNodes = 0;
    for (const auto& [vn, deg] : in_degree) {
        (void)deg;
        totalVNodes++;
    }

    if (sorted.size() == totalVNodes) return;  // no cycles

    // Collect cycle DFGNodes (map virtual nodes back)
    std::set<const DFGNode*> cycleNodes;
    for (const auto& [vn, deg] : in_degree) {
        if (!sorted.count(vn)) {
            cycleNodes.insert(vn.first);
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
        for (const auto& input : node->in) {
            if (cycleNodes.count(input.node)) {
                if (!loopInputs.empty()) loopInputs += ", ";
                loopInputs += input.node->str();
            }
        }
        msg += std::format("    {} <- [{}]\n", node->str(), loopInputs);
    }

    throw CompilerError(msg);
}

} // namespace custom_hdl
