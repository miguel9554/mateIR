#include "frontends/systemverilog/passes/dce.h"

#include <unordered_set>
#include <vector>

namespace mate {

bool eliminateDeadCode(DFG& graph,
                       const std::unordered_set<DFGNode*>& extraRoots) {
    // Phase 1: Mark — walk backward from roots collecting alive set
    std::unordered_set<DFGNode*> alive;
    std::vector<DFGNode*> worklist;

    // Roots: graph outputs.
    // .d OUTPUT nodes are in the outputs map and are automatically rooted.
    graph.forEachGraphOutput([&](const auto&, DFGNode* node) {
        if (alive.insert(node).second) worklist.push_back(node);
    });
    for (auto* node : extraRoots) {
        if (node && alive.insert(node).second) worklist.push_back(node);
    }

    // BFS/DFS through in edges
    while (!worklist.empty()) {
        DFGNode* current = worklist.back();
        worklist.pop_back();
        DFGTraversal::forEachInput(current, [&](size_t, const DFGOutput& input) {
            if (input.node && alive.insert(input.node).second) {
                worklist.push_back(input.node);
            }
        });
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

} // namespace mate
