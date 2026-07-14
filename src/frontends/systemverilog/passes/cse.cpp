#include "frontends/systemverilog/passes/cse.h"

#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mate {

namespace {

// Post-order over nodes reachable from graph outputs and extra roots.
// Unreachable (orphaned) nodes are deliberately not visited: merging a live
// node into an orphaned canonical would be legal but pointless, and skipping
// them matches constant_fold's traversal.
void postOrderVisit(DFGNode* node,
                    std::unordered_set<DFGNode*>& visited,
                    std::vector<DFGNode*>& order) {
    if (!node || visited.count(node)) return;
    visited.insert(node);
    DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
        postOrderVisit(input.node, visited, order);
    });
    order.push_back(node);
}

void appendTypeSignature(std::string& key, const DFGNode* node) {
    if (!node->hasType()) {
        key += "|t?";
        return;
    }
    const Type& type = *node->type;
    key += std::format("|t{}:{}:{}", static_cast<int>(type.kind), type.width,
                       type.isSigned() ? 1 : 0);
    for (const auto& dim : type.packed_dims) {
        key += std::format(",{}:{}", dim.left, dim.right);
    }
}

// Structural key for a node. Input pointers are resolved through the pending
// replacement map, so duplicates whose operands are themselves duplicates
// still key identically (the walk is post-order: producers resolve first).
// Returns empty when the node must not participate in CSE.
std::string nodeKey(const DFGNode* node,
                    const std::unordered_map<const DFGNode*, DFGNode*>& replacement) {
    const DFGOp kind = node->kind();
    switch (kind) {
        case DFGOp::INPUT:
        case DFGOp::OUTPUT:
        case DFGOp::SIGNAL:
        case DFGOp::X:
            // Boundary/named nodes carry identity; X values are independent
            // unknowns. Never merge.
            return {};
        case DFGOp::CONST: {
            // Hash-cons integer constants by value + type. Non-integer types
            // (enum/struct payloads) carry metadata the key doesn't cover.
            if (node->hasType() && node->type->kind != TypeKind::Integer) return {};
            std::string key = std::format("C{}", node->constValue());
            appendTypeSignature(key, node);
            return key;
        }
        default: {
            std::string key = std::format("O{}", static_cast<int>(kind));
            DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
                const DFGNode* producer = input.node;
                if (auto it = replacement.find(producer); it != replacement.end()) {
                    producer = it->second;
                }
                key += std::format("|{}:{}", static_cast<const void*>(producer), input.port);
            });
            if (kind == DFGOp::MUX) {
                key += "|m";
                for (int64_t value : node->muxValues()) {
                    key += std::format(",{}", value);
                }
            }
            if (kind == DFGOp::SLICE) {
                key += "|s";
                for (int64_t bit : node->sliceIndices()) {
                    key += std::format(",{}", bit);
                }
            }
            appendTypeSignature(key, node);
            return key;
        }
    }
}

} // namespace

bool eliminateCommonSubexpressions(DFG& graph,
                                   const std::unordered_set<DFGNode*>& extraRoots) {
    std::unordered_set<DFGNode*> visited;
    std::vector<DFGNode*> order;
    graph.forEachGraphOutput([&](const auto&, DFGNode* node) {
        postOrderVisit(node, visited, order);
    });
    for (auto* node : extraRoots) postOrderVisit(node, visited, order);

    // Decide all merges first (post-order guarantees operands resolve before
    // consumers), then rewrite inputs in one sweep: per-merge
    // redirectConsumers would rescan the whole graph each time.
    std::unordered_map<std::string, DFGNode*> canonical_for_key;
    std::unordered_map<const DFGNode*, DFGNode*> replacement;
    for (DFGNode* node : order) {
        std::string key = nodeKey(node, replacement);
        if (key.empty()) continue;
        auto [it, inserted] = canonical_for_key.try_emplace(std::move(key), node);
        if (inserted) continue;
        // Named nodes may be referenced by name-based lookups (named
        // constants, debug specs); they can be canonical but never a victim.
        if (!node->name.empty()) continue;
        replacement.emplace(node, it->second);
    }
    if (replacement.empty()) return false;

    for (auto& node : graph.nodes) {
        // Collect first, mutate after: replaceInputAt rewrites the payload
        // forEachInput is iterating over.
        std::vector<std::pair<size_t, DFGNode*>> rewrites;
        DFGTraversal::forEachInput(node.get(), [&](size_t index, const DFGOutput& input) {
            auto it = replacement.find(input.node);
            if (it != replacement.end()) rewrites.emplace_back(index, it->second);
        });
        for (const auto& [index, canonical] : rewrites) {
            node->replaceInputAt(index, DFGOutput(canonical));
        }
    }
    for (const auto& [victim, canonical] : replacement) {
        graph.replaceNodeInMaps(const_cast<DFGNode*>(victim), canonical);
    }
    return true;
}

} // namespace mate
