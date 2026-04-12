#include "passes/concat_cleanup.h"

#include <algorithm>
#include <format>

namespace custom_hdl {

bool cleanupConcats(DFG& graph) {
    bool changed = false;

    const size_t originalNodeCount = graph.nodes.size();
    for (size_t nodeIndex = 0; nodeIndex < originalNodeCount; ++nodeIndex) {
        DFGNode* node = graph.nodes[nodeIndex].get();
        if (node->kind() != DFGOp::CONCAT) continue;

        // If no inputs are CONCAT_ALIGN, this is a direct RHS concat — already clean
        const auto& concatParts = node->concatParts();
        bool hasAlign = std::any_of(concatParts.begin(), concatParts.end(),
            [](const DFGOutput& e) { return e.node->kind() == DFGOp::CONCAT_ALIGN; });
        if (!hasAlign) continue;

        // Collect CONCAT_ALIGN inputs with their positional info
        struct AlignInfo {
            int64_t high;
            int64_t low;
            DFGNode* expr;
            size_t order;
        };
        std::vector<AlignInfo> parts;

        for (size_t i = 0; i < concatParts.size(); ++i) {
            const auto& input = concatParts[i];
            DFGNode* child = input.node;
            if (child->kind() != DFGOp::CONCAT_ALIGN) {
                throw CompilerError(std::format(
                    "concat_cleanup: CONCAT input {} is not CONCAT_ALIGN",
                    child->str()), child);
            }
            auto align = child->concatAlignInputs();
            DFGNode* highNode = align.high.node;
            DFGNode* lowNode = align.low.node;
            if (highNode->kind() != DFGOp::CONST || lowNode->kind() != DFGOp::CONST) {
                throw CompilerError(std::format(
                    "concat_cleanup: CONCAT_ALIGN {} has non-constant indices",
                    child->str()), child);
            }
            int64_t high = highNode->constValue();
            int64_t low = lowNode->constValue();
            if (high < low) std::swap(high, low);
            parts.push_back({high, low, align.expr.node, i});
        }

        // Later CONCAT_ALIGN inputs are later writes to the same aggregate. If
        // they fully cover earlier default/partial slices, drop the covered
        // slices instead of leaving an over-wide concat for the simulator to
        // truncate.
        std::vector<AlignInfo> filtered;
        for (size_t i = 0; i < parts.size(); ++i) {
            bool covered = false;
            for (size_t j = i + 1; j < parts.size(); ++j) {
                bool overlaps = !(parts[i].high < parts[j].low || parts[i].low > parts[j].high);
                if (!overlaps) continue;

                bool fullyCovered = parts[j].low <= parts[i].low && parts[j].high >= parts[i].high;
                if (fullyCovered) {
                    covered = true;
                    break;
                }

                throw CompilerError(std::format(
                    "concat_cleanup: partial overlap between CONCAT_ALIGN ranges [{}:{}] and [{}:{}]",
                    parts[i].high, parts[i].low, parts[j].high, parts[j].low), node);
            }
            if (!covered) filtered.push_back(parts[i]);
        }
        parts = std::move(filtered);

        // Sort by descending high index (MSB first)
        std::sort(parts.begin(), parts.end(), [](const AlignInfo& a, const AlignInfo& b) {
            if (a.high != b.high) return a.high > b.high;
            return a.order < b.order;
        });

        // Replace CONCAT's input list with sorted expression nodes (unwrap CONCAT_ALIGN)
        std::vector<DFGOutput> newParts;
        newParts.reserve(parts.size());
        for (const auto& part : parts) {
            newParts.push_back(part.expr);
        }
        node->setConcatParts(newParts);

        changed = true;
    }

    return changed;
}

} // namespace custom_hdl
