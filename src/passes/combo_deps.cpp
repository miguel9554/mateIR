#include "passes/combo_deps.h"

#include <set>
#include <string>

namespace custom_hdl {

namespace {

// Find a child ResolvedModule by module type name
const ResolvedModule* findSubModule(const ResolvedModule& parent,
                                     const std::string& typeName) {
    for (const auto& sub : parent.hierarchyInstantiation) {
        if (sub.name == typeName) return &sub;
    }
    return nullptr;
}

// Backward DFS from a node, collecting reachable INPUT node names.
// visited: prevents infinite loops (should not happen in DAG, but defensive).
void collectInputDeps(const DFGNode* node, int arrivalPort,
                      const ResolvedModule& module,
                      std::set<std::string>& reachableInputs,
                      std::set<const DFGNode*>& visited) {
    if (!node || !visited.insert(node).second) return;

    if (node->op == DFGOp::INPUT) {
        reachableInputs.insert(node->name);
        return;
    }

    // Stop at constants — no combinational dependency
    if (node->op == DFGOp::CONST) return;

    // Stop at flop .q signals — these break combinational paths
    if (node->op == DFGOp::SIGNAL && node->name.ends_with(".q")) return;

    // MODULE nodes: use combo_deps to follow only relevant inputs
    if (node->op == DFGOp::MODULE) {
        const std::string& moduleType = std::get<std::string>(node->data);
        const ResolvedModule* sub = findSubModule(module, moduleType);

        if (sub && !sub->combo_deps.empty()) {
            // Find which output port we arrived at
            std::string outputPortName;
            if (arrivalPort >= 0 &&
                arrivalPort < static_cast<int>(node->output_names.size())) {
                outputPortName = node->output_names[arrivalPort];
            }

            if (!outputPortName.empty()) {
                auto it = sub->combo_deps.find(outputPortName);
                if (it != sub->combo_deps.end()) {
                    // Only recurse through the input ports this output depends on
                    for (const auto& inputPort : it->second) {
                        int idx = node->input_index(inputPort);
                        if (idx >= 0 && idx < static_cast<int>(node->in.size())) {
                            collectInputDeps(node->in[idx].node,
                                             node->in[idx].port,
                                             module, reachableInputs, visited);
                        }
                    }
                }
                // If output not in combo_deps, it has no combinational deps
                return;
            }
        }

        // Fallback: conservative — all inputs feed this output
        for (const auto& inp : node->in) {
            collectInputDeps(inp.node, inp.port, module,
                             reachableInputs, visited);
        }
        return;
    }

    // All other nodes: recurse into all inputs
    for (const auto& inp : node->in) {
        collectInputDeps(inp.node, inp.port, module,
                         reachableInputs, visited);
    }
}

} // anonymous namespace

void computeComboDeps(ResolvedModule& module) {
    if (!module.dfg) return;

    module.combo_deps.clear();

    // For each OUTPUT node, backward-DFS to find reachable INPUT nodes
    for (const auto& [outName, outNode] : module.dfg->outputs) {
        std::set<std::string> reachableInputs;
        std::set<const DFGNode*> visited;

        // OUTPUT nodes have a single driver in in[0]
        if (!outNode->in.empty()) {
            collectInputDeps(outNode->in[0].node, outNode->in[0].port,
                             module, reachableInputs, visited);
        }

        module.combo_deps[outName] = std::move(reachableInputs);
    }
}

void computeComboDepsBU(ResolvedModule& module) {
    // Process children first (bottom-up)
    for (auto& sub : module.hierarchyInstantiation) {
        computeComboDepsBU(sub);
    }
    computeComboDeps(module);
}

} // namespace custom_hdl
