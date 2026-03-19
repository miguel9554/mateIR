#include "passes/combo_deps.h"

#include <set>
#include <string>

namespace custom_hdl {

namespace {

// Backward DFS from a node, collecting reachable INPUT node names.
// visited: prevents infinite loops (should not happen in DAG, but defensive).
void collectInputDeps(const DFGNode* node,
                      std::set<std::string>& reachableInputs,
                      std::set<const DFGNode*>& visited) {
    if (!node || !visited.insert(node).second) return;

    // INPUT nodes (includes .q flop state nodes) are the leaves
    if (node->op == DFGOp::INPUT) {
        reachableInputs.insert(node->name);
        return;
    }

    // Stop at constants — no combinational dependency
    if (node->op == DFGOp::CONST) return;

    // All other nodes: recurse into all inputs
    for (const auto& inp : node->in) {
        collectInputDeps(inp.node, reachableInputs, visited);
    }
}

} // anonymous namespace

void computeComboDeps(ResolvedModule& module) {
    if (!module.dfg) return;

    module.combo_deps.clear();

    // For each OUTPUT node, backward-DFS to find reachable INPUT nodes
    for (const auto& [outName, outNode] : module.dfg->getOutputsMap()) {
        std::set<std::string> reachableInputs;
        std::set<const DFGNode*> visited;

        // OUTPUT nodes have a single driver in in[0]
        if (!outNode->in.empty()) {
            collectInputDeps(outNode->in[0].node, reachableInputs, visited);
        }

        module.combo_deps[outName] = std::move(reachableInputs);
    }
}

} // namespace custom_hdl
