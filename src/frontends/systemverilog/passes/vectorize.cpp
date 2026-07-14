#include "frontends/systemverilog/passes/vectorize.h"

#include "frontends/systemverilog/passes/constant_fold.h"

#include <vector>

namespace mate {

namespace {

// Lanes are LSB-first throughout this pass; CONCAT parts are MSB-first, so
// lane i corresponds to part (N-1-i).
std::vector<DFGNode*> lanesOf(const DFGNode* concat) {
    const auto& parts = concat->concatParts();
    std::vector<DFGNode*> lanes;
    lanes.reserve(parts.size());
    for (size_t i = parts.size(); i-- > 0;) lanes.push_back(parts[i].node);
    return lanes;
}

int typedWidth(const DFGNode* node) {
    return node->hasType() ? node->type->width : -1;
}

void adoptIdentity(DFGNode* fresh, const DFGNode* original) {
    fresh->loc = original->loc;
    fresh->instance_path = original->instance_path;
}

// Build one node carrying the lane values as a packed vector (lane i at bits
// [w*(i+1)-1 : w*i]). Materialized CONCATs are picked up again by the
// fixpoint, which is how deeper cone levels vectorize.
DFGNode* materializeVector(DFG& graph, const std::vector<DFGNode*>& lanes,
                           int lane_width, const DFGNode* site) {
    const int total = lane_width * static_cast<int>(lanes.size());

    bool all_same = true;
    bool all_const = true;
    for (DFGNode* lane : lanes) {
        all_same &= lane == lanes.front();
        all_const &= lane->kind() == DFGOp::CONST;
    }

    if (all_same && lanes.size() > 1) {
        // Replication: a SLICE with repeated indices (result bit w*i+j reads
        // source bit j).
        std::vector<int64_t> indices;
        indices.reserve(static_cast<size_t>(total));
        for (size_t i = 0; i < lanes.size(); ++i) {
            for (int j = 0; j < lane_width; ++j) indices.push_back(j);
        }
        DFGNode* replicated = graph.slice(lanes.front(), std::move(indices));
        replicated->type = Type::makeInteger(total, false);
        adoptIdentity(replicated, site);
        return replicated;
    }

    if (all_const && total <= 63) {
        int64_t value = 0;
        for (size_t i = lanes.size(); i-- > 0;) {
            const uint64_t lane_mask = (uint64_t{1} << lane_width) - 1;
            value = (value << lane_width) |
                    static_cast<int64_t>(static_cast<uint64_t>(lanes[i]->constValue()) & lane_mask);
        }
        DFGNode* packed = graph.constant(value);
        packed->type = Type::makeInteger(total, false);
        adoptIdentity(packed, site);
        return packed;
    }

    std::vector<DFGNode*> parts(lanes.rbegin(), lanes.rend());
    DFGNode* vec = graph.concat(parts);
    vec->type = Type::makeInteger(total, false);
    adoptIdentity(vec, site);
    return vec;
}

// R1: every lane is a SLICE of one common source -> merge the index lists.
bool trySliceMerge(DFG& graph, DFGNode* concat, const std::vector<DFGNode*>& lanes) {
    DFGNode* source = nullptr;
    for (DFGNode* lane : lanes) {
        if (lane->kind() != DFGOp::SLICE) return false;
        // Element-peel-typed slices (typed wider than their index count, the
        // open NZB inconsistency) don't merge cleanly; skip them.
        if (typedWidth(lane) != static_cast<int>(lane->sliceIndices().size())) return false;
        DFGNode* lane_source = lane->sliceSource().node;
        if (!source) source = lane_source;
        if (lane_source != source) return false;
    }

    std::vector<int64_t> merged;
    for (DFGNode* lane : lanes) {
        const auto& indices = lane->sliceIndices();
        merged.insert(merged.end(), indices.begin(), indices.end());
    }

    // Identity gather of the full source is the source itself.
    if (source->hasType() && static_cast<int>(merged.size()) == source->type->width &&
        !source->type->isSigned()) {
        bool identity = true;
        for (size_t j = 0; j < merged.size(); ++j) identity &= merged[j] == static_cast<int64_t>(j);
        if (identity) {
            graph.redirectConsumers(concat, source);
            return true;
        }
    }

    DFGNode* fused = graph.slice(source, std::move(merged));
    fused->type = concat->type;
    adoptIdentity(fused, concat);
    graph.redirectConsumers(concat, fused);
    return true;
}

bool isVectorizableBitwise(DFGOp op) {
    // Lane-independent ops only: anything with carries (ADD/SUB/MUL) or
    // cross-lane reads (shifts, reductions, compares) must not merge.
    switch (op) {
        case DFGOp::BITWISE_NOT:
        case DFGOp::BITWISE_AND:
        case DFGOp::BITWISE_OR:
        case DFGOp::BITWISE_XOR:
        case DFGOp::BITWISE_XNOR:
            return true;
        default:
            return false;
    }
}

// R2: every lane is the same bitwise op with the same lane width -> one wide
// op over materialized operand vectors.
bool tryBitwiseMerge(DFG& graph, DFGNode* concat, const std::vector<DFGNode*>& lanes) {
    const DFGOp op = lanes.front()->kind();
    if (!isVectorizableBitwise(op)) return false;
    const int lane_width = typedWidth(lanes.front());
    if (lane_width <= 0) return false;
    for (DFGNode* lane : lanes) {
        if (lane->kind() != op || typedWidth(lane) != lane_width) return false;
    }

    const int total = lane_width * static_cast<int>(lanes.size());
    DFGNode* wide = nullptr;
    if (op == DFGOp::BITWISE_NOT) {
        std::vector<DFGNode*> operands;
        operands.reserve(lanes.size());
        for (DFGNode* lane : lanes) operands.push_back(lane->unaryInputs().operand.node);
        DFGNode* vec = materializeVector(graph, operands, lane_width, concat);
        wide = graph.bitwiseNot(vec);
    } else {
        std::vector<DFGNode*> lhs, rhs;
        lhs.reserve(lanes.size());
        rhs.reserve(lanes.size());
        for (DFGNode* lane : lanes) {
            auto inputs = lane->binaryInputs();
            lhs.push_back(inputs.lhs.node);
            rhs.push_back(inputs.rhs.node);
        }
        DFGNode* lvec = materializeVector(graph, lhs, lane_width, concat);
        DFGNode* rvec = materializeVector(graph, rhs, lane_width, concat);
        switch (op) {
            case DFGOp::BITWISE_AND:  wide = graph.bitwiseAnd(lvec, rvec); break;
            case DFGOp::BITWISE_OR:   wide = graph.bitwiseOr(lvec, rvec); break;
            case DFGOp::BITWISE_XOR:  wide = graph.bitwiseXor(lvec, rvec); break;
            case DFGOp::BITWISE_XNOR: wide = graph.bitwiseXnor(lvec, rvec); break;
            default: return false;
        }
    }
    wide->type = Type::makeInteger(total, false);
    adoptIdentity(wide, concat);
    graph.redirectConsumers(concat, wide);
    return true;
}

// R3a: every lane is a MUX on one shared selector with identical selector
// values -> one wide MUX with vectorized arms.
bool trySharedSelectorMux(DFG& graph, DFGNode* concat, const std::vector<DFGNode*>& lanes) {
    DFGNode* selector = lanes.front()->muxSelector().node;
    const std::vector<int64_t> values = lanes.front()->muxValues();
    const int lane_width = typedWidth(lanes.front());
    if (lane_width <= 0) return false;
    for (DFGNode* lane : lanes) {
        if (lane->muxSelector().node != selector) return false;
        if (lane->muxValues() != values) return false;
        if (typedWidth(lane) != lane_width) return false;
    }

    const int total = lane_width * static_cast<int>(lanes.size());
    std::vector<DFGNode*> arm_vectors;
    arm_vectors.reserve(values.size());
    for (size_t a = 0; a < values.size(); ++a) {
        std::vector<DFGNode*> arm_lanes;
        arm_lanes.reserve(lanes.size());
        for (DFGNode* lane : lanes) arm_lanes.push_back(lane->muxArmData(a).node);
        arm_vectors.push_back(materializeVector(graph, arm_lanes, lane_width, concat));
    }
    DFGNode* wide = graph.mux(selector, values, arm_vectors);
    wide->type = Type::makeInteger(total, false);
    adoptIdentity(wide, concat);
    graph.redirectConsumers(concat, wide);
    return true;
}

// R3b: every lane is a 2-arm MUX with its own 1-bit selector -> masked merge
// (mask & taken) | (~mask & fallthrough), expanding the mask per lane bit via
// a repeated-index SLICE when lanes are wider than one bit.
bool tryMaskedMerge(DFG& graph, DFGNode* concat, const std::vector<DFGNode*>& lanes) {
    const int lane_width = typedWidth(lanes.front());
    if (lane_width <= 0) return false;
    for (DFGNode* lane : lanes) {
        if (!lane->isBinaryMux()) return false;
        if (typedWidth(lane) != lane_width) return false;
        DFGNode* selector = lane->muxSelector().node;
        if (typedWidth(selector) != 1) return false;
    }

    const int total = lane_width * static_cast<int>(lanes.size());
    std::vector<DFGNode*> selectors, taken, fallthrough;
    selectors.reserve(lanes.size());
    taken.reserve(lanes.size());
    fallthrough.reserve(lanes.size());
    for (DFGNode* lane : lanes) {
        selectors.push_back(lane->muxSelector().node);
        taken.push_back(lane->muxDataForValue(1));
        fallthrough.push_back(lane->muxDataForValue(0));
    }

    DFGNode* mask = materializeVector(graph, selectors, 1, concat);
    if (lane_width > 1) {
        std::vector<int64_t> expand;
        expand.reserve(static_cast<size_t>(total));
        for (size_t i = 0; i < lanes.size(); ++i) {
            for (int j = 0; j < lane_width; ++j) expand.push_back(static_cast<int64_t>(i));
        }
        DFGNode* expanded = graph.slice(mask, std::move(expand));
        expanded->type = Type::makeInteger(total, false);
        adoptIdentity(expanded, concat);
        mask = expanded;
    }

    DFGNode* taken_vec = materializeVector(graph, taken, lane_width, concat);
    DFGNode* fall_vec = materializeVector(graph, fallthrough, lane_width, concat);
    DFGNode* inverted = graph.bitwiseNot(mask);
    inverted->type = Type::makeInteger(total, false);
    DFGNode* on_taken = graph.bitwiseAnd(mask, taken_vec);
    on_taken->type = Type::makeInteger(total, false);
    DFGNode* on_fall = graph.bitwiseAnd(inverted, fall_vec);
    on_fall->type = Type::makeInteger(total, false);
    DFGNode* merged = graph.bitwiseOr(on_taken, on_fall);
    merged->type = Type::makeInteger(total, false);
    for (DFGNode* fresh : {inverted, on_taken, on_fall, merged}) adoptIdentity(fresh, concat);
    graph.redirectConsumers(concat, merged);
    return true;
}

bool tryVectorizeConcat(DFG& graph, DFGNode* concat) {
    const auto lanes = lanesOf(concat);
    if (lanes.size() < 2) return false;
    if (!concat->hasType()) return false;
    const DFGOp op = lanes.front()->kind();
    for (DFGNode* lane : lanes) {
        if (lane->kind() != op || !lane->hasType()) return false;
    }
    switch (op) {
        case DFGOp::SLICE:
            return trySliceMerge(graph, concat, lanes);
        case DFGOp::MUX:
            if (trySharedSelectorMux(graph, concat, lanes)) return true;
            return tryMaskedMerge(graph, concat, lanes);
        default:
            return tryBitwiseMerge(graph, concat, lanes);
    }
}

} // namespace

bool vectorizeDFG(DFG& graph, const std::unordered_set<DFGNode*>& extraRoots) {
    bool any_changed = false;
    bool changed;
    size_t sweeps = 0;
    do {
        if (++sweeps > 1000) {
            throw CompilerError("vectorize: fixpoint did not converge after 1000 sweeps");
        }
        changed = false;
        // Snapshot: rewrites append nodes; new CONCATs are visited on the
        // next fixpoint iteration. Only live CONCATs (ones some node still
        // consumes) are candidates — replaced concats stay in the node list
        // until DCE and would otherwise match forever.
        std::unordered_set<const DFGNode*> consumed;
        for (const auto& node : graph.nodes) {
            DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
                consumed.insert(input.node);
            });
        }
        std::vector<DFGNode*> snapshot;
        snapshot.reserve(graph.nodes.size());
        for (const auto& node : graph.nodes) snapshot.push_back(node.get());
        for (DFGNode* node : snapshot) {
            if (node->kind() != DFGOp::CONCAT) continue;
            if (!consumed.count(node)) continue;
            if (tryVectorizeConcat(graph, node)) changed = true;
        }
        if (changed) {
            // Pack const operand vectors, fold replicated-constant slices,
            // and let algebraic MUX/identity rules clean up before the next
            // sweep sees the graph.
            constantFold(graph, extraRoots);
        }
        any_changed |= changed;
    } while (changed);
    return any_changed;
}

} // namespace mate
