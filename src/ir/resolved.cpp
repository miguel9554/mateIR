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
// Combinational loop detection
// ============================================================================

void validateNoCombLoops(const ResolvedModule& module) {
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
        for (const auto& input : node->in) {
            in_degree[node.get()]++;
            successors[input.node].push_back(node.get());
        }
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
