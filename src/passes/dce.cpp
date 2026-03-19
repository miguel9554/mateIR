#include "passes/dce.h"

#include <unordered_set>
#include <vector>

namespace custom_hdl {

bool eliminateDeadCode(DFG& graph) {
    // Phase 1: Mark — walk backward from roots collecting alive set
    std::unordered_set<DFGNode*> alive;
    std::vector<DFGNode*> worklist;

    // Roots: outputs, signals, MODULE nodes
    for (auto& [name, node] : graph.getOutputsMap()) {
        if (alive.insert(node).second) worklist.push_back(node);
    }
    for (auto& [name, node] : graph.getSignalsMap()) {
        if (alive.insert(node).second) worklist.push_back(node);
    }
    for (auto& node : graph.nodes) {
        if (node->op == DFGOp::MODULE) {
            if (alive.insert(node.get()).second) worklist.push_back(node.get());
        }
    }

    // BFS/DFS through in edges
    while (!worklist.empty()) {
        DFGNode* current = worklist.back();
        worklist.pop_back();
        for (auto& input : current->in) {
            if (input.node && alive.insert(input.node).second) {
                worklist.push_back(input.node);
            }
        }
    }

    // Phase 2: Sweep — remove dead nodes
    size_t before = graph.nodes.size();

    std::erase_if(graph.nodes, [&alive](const std::unique_ptr<DFGNode>& n) {
        return !alive.count(n.get());
    });

    // Clean up named maps
    graph.pruneByAliveSet(alive);

    return graph.nodes.size() < before;
}

} // namespace custom_hdl
