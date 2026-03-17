#include "passes/concat_cleanup.h"

#include <algorithm>
#include <format>

namespace custom_hdl {

bool cleanupConcats(DFG& graph) {
    bool changed = false;

    for (auto& nodePtr : graph.nodes) {
        if (nodePtr->op != DFGOp::CONCAT) continue;

        // If no inputs are CONCAT_ALIGN, this is a direct RHS concat — already clean
        bool hasAlign = std::any_of(nodePtr->in.begin(), nodePtr->in.end(),
            [](const DFGOutput& e) { return e.node->op == DFGOp::CONCAT_ALIGN; });
        if (!hasAlign) continue;

        // Collect CONCAT_ALIGN inputs with their positional info
        struct AlignInfo {
            int64_t high;
            int64_t low;
            DFGNode* expr;
        };
        std::vector<AlignInfo> parts;

        for (auto& input : nodePtr->in) {
            DFGNode* child = input.node;
            if (child->op != DFGOp::CONCAT_ALIGN) {
                throw CompilerError(std::format(
                    "concat_cleanup: CONCAT input {} is not CONCAT_ALIGN",
                    child->str()), child);
            }
            if (child->in.size() != 3) {
                throw CompilerError(std::format(
                    "concat_cleanup: CONCAT_ALIGN {} has {} inputs (expected 3)",
                    child->str(), child->in.size()), child);
            }
            DFGNode* highNode = child->in[1].node;
            DFGNode* lowNode = child->in[2].node;
            if (highNode->op != DFGOp::CONST || lowNode->op != DFGOp::CONST) {
                throw CompilerError(std::format(
                    "concat_cleanup: CONCAT_ALIGN {} has non-constant indices",
                    child->str()), child);
            }
            int64_t high = std::get<int64_t>(highNode->data);
            int64_t low = std::get<int64_t>(lowNode->data);
            parts.push_back({high, low, child->in[0].node});
        }

        // Sort by descending high index (MSB first)
        std::sort(parts.begin(), parts.end(), [](const AlignInfo& a, const AlignInfo& b) {
            return a.high > b.high;
        });

        // Replace CONCAT's input list with sorted expression nodes (unwrap CONCAT_ALIGN)
        nodePtr->in.clear();
        for (const auto& part : parts) {
            nodePtr->in.push_back(part.expr);
        }

        changed = true;
    }

    return changed;
}

} // namespace custom_hdl
