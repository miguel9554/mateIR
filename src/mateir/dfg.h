#pragma once

#include "mateir/types.h"
#include "util/source_loc.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>
#include <string>
#include <variant>

namespace mate {

enum class DFGOp {
    // Identity ops
    INPUT,      // Graph source boundary node (module inputs + flop .q leaves)
    OUTPUT,     // Graph sink boundary node (module outputs + flop .d leaves)
    SIGNAL,     // Internal signal (named placeholder)
    CONST,      // Constant value (data: int64_t)
    // Arithmetic ops
    ADD,
    SUB,
    MUL,
    UNARY_NEGATE,
    // Bit extraction / concatenation ops
    SLICE,      // Static bit-slice: in[0]=source, in[1]=high (CONST), in[2]=low (CONST)
    CONCAT,     // Concatenation: in[0..N-1] = parts, MSB-first
    // MUX
    MUX,        // Exhaustive value mux: in[0]=sel, in[1..]=data arms keyed by mux_values
    // Comparison ops
    EQ,         // Equal (==)
    LT,         // Less than (<)
    LE,         // Less or equal (<=)
    GT,         // Greater than (>)
    GE,         // Greater or equal (>=)
    // Shift ops
    SHL,        // Arithmetic shift left (<<<)
    ASR,        // Arithmetic shift right (>>>)
    // Binary bitwise/logical ops
    BITWISE_NOT,
    BITWISE_AND,
    BITWISE_OR,
    BITWISE_XOR,
    BITWISE_XNOR,
    REDUCTION_AND,
    REDUCTION_NAND,
    REDUCTION_OR,
    REDUCTION_NOR,
    REDUCTION_XOR,
    REDUCTION_XNOR,
};

inline const char* to_string(DFGOp op) {
    switch (op) {
        case DFGOp::INPUT: return "INPUT";
        case DFGOp::OUTPUT: return "OUTPUT";
        case DFGOp::SIGNAL: return "SIGNAL";
        case DFGOp::CONST: return "CONST";
        case DFGOp::SLICE: return "SLICE";
        case DFGOp::CONCAT: return "CONCAT";
        case DFGOp::ADD: return "ADD";
        case DFGOp::SUB: return "SUB";
        case DFGOp::MUL: return "MUL";
        case DFGOp::EQ: return "EQ";
        case DFGOp::LT: return "LT";
        case DFGOp::LE: return "LE";
        case DFGOp::GT: return "GT";
        case DFGOp::GE: return "GE";
        case DFGOp::SHL: return "SHL";
        case DFGOp::ASR: return "ASR";
        case DFGOp::MUX: return "MUX";
        case DFGOp::UNARY_NEGATE: return "UNARY_NEGATE";
        case DFGOp::BITWISE_NOT: return "BITWISE_NOT";
        case DFGOp::BITWISE_AND:  return "BITWISE_AND";
        case DFGOp::BITWISE_OR:   return "BITWISE_OR";
        case DFGOp::BITWISE_XOR:  return "BITWISE_XOR";
        case DFGOp::BITWISE_XNOR: return "BITWISE_XNOR";
        case DFGOp::REDUCTION_AND: return "REDUCTION_AND";
        case DFGOp::REDUCTION_NAND: return "REDUCTION_NAND";
        case DFGOp::REDUCTION_OR: return "REDUCTION_OR";
        case DFGOp::REDUCTION_NOR: return "REDUCTION_NOR";
        case DFGOp::REDUCTION_XOR: return "REDUCTION_XOR";
        case DFGOp::REDUCTION_XNOR: return "REDUCTION_XNOR";
    }
    return "UNKNOWN";
}

// Returns the expected number of inputs for a given op.
// -1 means variable (validated separately).
inline int expectedInputs(DFGOp op) {
    switch (op) {
        case DFGOp::INPUT:  return 0;
        case DFGOp::CONST:  return 0;
        // OUTPUT: 0 (undriven) or 1 (driven) — validated separately
        case DFGOp::OUTPUT: return -1;
        // SIGNAL: 0 (undriven during construction) or 1 (driven) — validated separately
        case DFGOp::SIGNAL: return -1;
        // MUX: variable input count, validated separately
        case DFGOp::MUX:    return -1;

        case DFGOp::SLICE:  return 3;
        case DFGOp::CONCAT: return -1; // variable
        case DFGOp::ADD:    return 2;
        case DFGOp::SUB:    return 2;
        case DFGOp::MUL:    return 2;
        case DFGOp::EQ:     return 2;
        case DFGOp::LT:     return 2;
        case DFGOp::LE:     return 2;
        case DFGOp::GT:     return 2;
        case DFGOp::GE:     return 2;
        case DFGOp::SHL:    return 2;
        case DFGOp::ASR:    return 2;

        case DFGOp::UNARY_NEGATE:    return 1;
        case DFGOp::BITWISE_NOT:     return 1;
        case DFGOp::BITWISE_AND:  return 2;
        case DFGOp::BITWISE_OR:   return 2;
        case DFGOp::BITWISE_XOR:  return 2;
        case DFGOp::BITWISE_XNOR: return 2;
        case DFGOp::REDUCTION_AND:   return 1;
        case DFGOp::REDUCTION_NAND:  return 1;
        case DFGOp::REDUCTION_OR:    return 1;
        case DFGOp::REDUCTION_NOR:   return 1;
        case DFGOp::REDUCTION_XOR:   return 1;
        case DFGOp::REDUCTION_XNOR:  return 1;
    }
    return -1;
}

} // namespace mate

template<>
struct std::formatter<mate::DFGOp> : std::formatter<const char*> {
    auto format(mate::DFGOp op, std::format_context& ctx) const {
        return std::formatter<const char*>::format(mate::to_string(op), ctx);
    }
};

namespace mate {

struct DFGNode; // forward declare
struct DFG;     // forward declare
struct DFGTraversal; // forward declare

struct DFGOutput {
    DFGNode* node;
    int port = 0;
    DFGOutput(DFGNode* n, int p = 0) : node(n), port(p) {
        if (!n) throw CompilerError("DFGOutput: null node");
        if (p < 0) throw CompilerError("DFGOutput: negative port");
    }
};

struct DFGMuxArm {
    int64_t value;
    DFGOutput data;
};

struct DFGInputPayload {};
struct DFGOutputPayload { std::optional<DFGOutput> driver; };
struct DFGSignalPayload { std::optional<DFGOutput> driver; };
struct DFGConstPayload { int64_t value; };
struct DFGUnaryPayload { DFGOp op; DFGOutput operand; };
struct DFGBinaryPayload { DFGOp op; DFGOutput lhs; DFGOutput rhs; };
struct DFGSlicePayload { DFGOutput source; DFGOutput high; DFGOutput low; };
struct DFGConcatPayload { std::vector<DFGOutput> parts; };
struct DFGMuxPayload { DFGOutput selector; std::vector<DFGMuxArm> arms; };

struct DFGUnaryInputs {
    DFGOutput operand;
};

struct DFGBinaryInputs {
    DFGOutput lhs;
    DFGOutput rhs;
};

struct DFGSliceInputs {
    DFGOutput source;
    DFGOutput high;
    DFGOutput low;
};

using DFGPayload = std::variant<
    DFGInputPayload,
    DFGOutputPayload,
    DFGSignalPayload,
    DFGConstPayload,
    DFGUnaryPayload,
    DFGBinaryPayload,
    DFGSlicePayload,
    DFGConcatPayload,
    DFGMuxPayload
>;

inline bool isUnaryDFGOp(DFGOp op) {
    switch (op) {
        case DFGOp::UNARY_NEGATE:
        case DFGOp::BITWISE_NOT:
        case DFGOp::REDUCTION_AND:
        case DFGOp::REDUCTION_NAND:
        case DFGOp::REDUCTION_OR:
        case DFGOp::REDUCTION_NOR:
        case DFGOp::REDUCTION_XOR:
        case DFGOp::REDUCTION_XNOR:
            return true;
        default:
            return false;
    }
}

inline bool isBinaryDFGOp(DFGOp op) {
    switch (op) {
        case DFGOp::ADD:
        case DFGOp::SUB:
        case DFGOp::MUL:
        case DFGOp::EQ:
        case DFGOp::LT:
        case DFGOp::LE:
        case DFGOp::GT:
        case DFGOp::GE:
        case DFGOp::SHL:
        case DFGOp::ASR:
        case DFGOp::BITWISE_AND:
        case DFGOp::BITWISE_OR:
        case DFGOp::BITWISE_XOR:
        case DFGOp::BITWISE_XNOR:
            return true;
        default:
            return false;
    }
};

struct DFGNode {
    std::string name;  // Optional name (empty = anonymous)

    // Hierarchical path set during DFG inlining (empty = top-level node).
    // For inlined sub-module nodes: "inst" or "inst.subinst"
    std::string instance_path;

    // Type info (width, signedness) — set on leaf nodes, propagated by type pass
    std::optional<Type> type;

    // Source location in the original Verilog (set during elaboration)
    std::optional<SourceLoc> loc;

private:
    DFGPayload payload_;
    mutable std::vector<DFGOutput> input_cache_;
    mutable std::vector<int64_t> mux_value_cache_;

public:
    struct ConstructionKey {
    private:
        friend struct DFG;
        ConstructionKey() = default;
    };

    bool hasType() const { return type.has_value(); }

    DFGOp kind() const {
        if (std::holds_alternative<DFGInputPayload>(payload_)) return DFGOp::INPUT;
        if (std::holds_alternative<DFGOutputPayload>(payload_)) return DFGOp::OUTPUT;
        if (std::holds_alternative<DFGSignalPayload>(payload_)) return DFGOp::SIGNAL;
        if (std::holds_alternative<DFGConstPayload>(payload_)) return DFGOp::CONST;
        if (auto* p = std::get_if<DFGUnaryPayload>(&payload_)) return p->op;
        if (auto* p = std::get_if<DFGBinaryPayload>(&payload_)) return p->op;
        if (std::holds_alternative<DFGSlicePayload>(payload_)) return DFGOp::SLICE;
        if (std::holds_alternative<DFGConcatPayload>(payload_)) return DFGOp::CONCAT;
        if (std::holds_alternative<DFGMuxPayload>(payload_)) return DFGOp::MUX;
        throw CompilerError("DFGNode: invalid payload", this);
    }

    DFGOp opValue() const { return kind(); }

private:
    const std::vector<DFGOutput>& rawInputs() const {
        input_cache_.clear();
        if (auto* p = std::get_if<DFGOutputPayload>(&payload_)) {
            if (p->driver) input_cache_.push_back(*p->driver);
        } else if (auto* p = std::get_if<DFGSignalPayload>(&payload_)) {
            if (p->driver) input_cache_.push_back(*p->driver);
        } else if (auto* p = std::get_if<DFGUnaryPayload>(&payload_)) {
            input_cache_.push_back(p->operand);
        } else if (auto* p = std::get_if<DFGBinaryPayload>(&payload_)) {
            input_cache_.push_back(p->lhs);
            input_cache_.push_back(p->rhs);
        } else if (auto* p = std::get_if<DFGSlicePayload>(&payload_)) {
            input_cache_ = {p->source, p->high, p->low};
        } else if (auto* p = std::get_if<DFGConcatPayload>(&payload_)) {
            input_cache_ = p->parts;
        } else if (auto* p = std::get_if<DFGMuxPayload>(&payload_)) {
            input_cache_.push_back(p->selector);
            for (const auto& arm : p->arms) input_cache_.push_back(arm.data);
        }
        return input_cache_;
    }

    friend struct DFG;
    friend struct DFGTraversal;

public:
    std::optional<DFGOutput> driver() const {
        if (const auto* p = std::get_if<DFGOutputPayload>(&payload_)) return p->driver;
        if (const auto* p = std::get_if<DFGSignalPayload>(&payload_)) return p->driver;
        throw CompilerError(std::format("driver: {} is not OUTPUT or SIGNAL", str()), this);
    }

    DFGUnaryInputs unaryInputs() const {
        const auto& p = std::get<DFGUnaryPayload>(payload_);
        return {p.operand};
    }

    DFGBinaryInputs binaryInputs() const {
        const auto& p = std::get<DFGBinaryPayload>(payload_);
        return {p.lhs, p.rhs};
    }

    DFGSliceInputs sliceInputs() const {
        const auto& p = std::get<DFGSlicePayload>(payload_);
        return {p.source, p.high, p.low};
    }

    const std::vector<DFGOutput>& concatParts() const {
        return std::get<DFGConcatPayload>(payload_).parts;
    }

    int64_t constValue() const { return std::get<DFGConstPayload>(payload_).value; }

    const std::vector<int64_t>& muxValues() const {
        mux_value_cache_.clear();
        for (const auto& arm : std::get<DFGMuxPayload>(payload_).arms) {
            mux_value_cache_.push_back(arm.value);
        }
        return mux_value_cache_;
    }

    int num_outputs() const { return 1; }

    DFGOutput output(int port = 0) { return {this, port}; }

    bool isMux() const { return opValue() == DFGOp::MUX; }
    DFGOutput muxSelector() const { return std::get<DFGMuxPayload>(payload_).selector; }
    size_t muxArmCount() const { return std::get<DFGMuxPayload>(payload_).arms.size(); }
    int64_t muxArmValue(size_t index) const { return std::get<DFGMuxPayload>(payload_).arms.at(index).value; }
    DFGOutput muxArmData(size_t index) const { return std::get<DFGMuxPayload>(payload_).arms.at(index).data; }

    int muxArmIndexForValue(int64_t value) const {
        const auto& arms = std::get<DFGMuxPayload>(payload_).arms;
        for (size_t i = 0; i < arms.size(); ++i) {
            if (arms[i].value == value) return static_cast<int>(i);
        }
        return -1;
    }

    DFGNode* muxDataForValue(int64_t value) const {
        int index = muxArmIndexForValue(value);
        return index >= 0 ? muxArmData(static_cast<size_t>(index)).node : nullptr;
    }

    void setMuxSelector(DFGOutput sel) {
        std::get<DFGMuxPayload>(payload_).selector = sel;
    }

    // Swap only data edges at arm positions i and j; mux_values stays fixed
    // so selector codes remain attached to their positions.
    // Use this when inverting the selector signal (e.g. MUX(!s,...) → MUX(s,...)).
    void swapMuxArmData(size_t i, size_t j) {
        auto& arms = std::get<DFGMuxPayload>(payload_).arms;
        std::swap(arms.at(i).data, arms.at(j).data);
    }

    bool isBinaryMux() const {
        return opValue() == DFGOp::MUX && muxArmCount() == 2 &&
               muxArmIndexForValue(0) >= 0 &&
               muxArmIndexForValue(1) >= 0;
    }

    DFGNode(ConstructionKey, DFGPayload payload) : payload_(std::move(payload)) {}
    DFGNode(ConstructionKey, DFGPayload payload, std::string nodeName)
        : name(std::move(nodeName)), payload_(std::move(payload)) {}

    void setDriver(DFGOutput driver) {
        if (auto* p = std::get_if<DFGOutputPayload>(&payload_)) {
            p->driver = driver;
            return;
        }
        if (auto* p = std::get_if<DFGSignalPayload>(&payload_)) {
            p->driver = driver;
            return;
        }
        throw CompilerError(std::format("setDriver: target {} is not OUTPUT or SIGNAL", str()), this);
    }

    void clearDriver() {
        if (auto* p = std::get_if<DFGOutputPayload>(&payload_)) {
            p->driver.reset();
            return;
        }
        if (auto* p = std::get_if<DFGSignalPayload>(&payload_)) {
            p->driver.reset();
            return;
        }
        throw CompilerError(std::format("clearDriver: target {} is not OUTPUT or SIGNAL", str()), this);
    }

    void rewriteToConst(int64_t value) {
        payload_ = DFGConstPayload{value};
        name.clear();
    }

    void rewriteToUnary(DFGOp newOp, DFGOutput operand) {
        if (!isUnaryDFGOp(newOp)) throw CompilerError("rewriteToUnary: op is not unary", this);
        payload_ = DFGUnaryPayload{newOp, operand};
    }

    void rewriteToBinary(DFGOp newOp, DFGOutput lhs, DFGOutput rhs) {
        if (!isBinaryDFGOp(newOp)) throw CompilerError("rewriteToBinary: op is not binary", this);
        payload_ = DFGBinaryPayload{newOp, lhs, rhs};
    }

    void replaceInputAt(size_t index, DFGOutput replacement) {
        if (auto* p = std::get_if<DFGOutputPayload>(&payload_)) {
            if (index != 0 || !p->driver) throw CompilerError("replaceInputAt: OUTPUT index out of range", this);
            p->driver = replacement; return;
        }
        if (auto* p = std::get_if<DFGSignalPayload>(&payload_)) {
            if (index != 0 || !p->driver) throw CompilerError("replaceInputAt: SIGNAL index out of range", this);
            p->driver = replacement; return;
        }
        if (auto* p = std::get_if<DFGUnaryPayload>(&payload_)) {
            if (index != 0) throw CompilerError("replaceInputAt: unary index out of range", this);
            p->operand = replacement; return;
        }
        if (auto* p = std::get_if<DFGBinaryPayload>(&payload_)) {
            if (index == 0) p->lhs = replacement;
            else if (index == 1) p->rhs = replacement;
            else throw CompilerError("replaceInputAt: binary index out of range", this);
            return;
        }
        if (auto* p = std::get_if<DFGSlicePayload>(&payload_)) {
            if (index == 0) p->source = replacement;
            else if (index == 1) p->high = replacement;
            else if (index == 2) p->low = replacement;
            else throw CompilerError("replaceInputAt: SLICE index out of range", this);
            return;
        }
        if (auto* p = std::get_if<DFGConcatPayload>(&payload_)) {
            p->parts.at(index) = replacement; return;
        }
        if (auto* p = std::get_if<DFGMuxPayload>(&payload_)) {
            if (index == 0) p->selector = replacement;
            else p->arms.at(index - 1).data = replacement;
            return;
        }
        throw CompilerError("replaceInputAt: node has no inputs", this);
    }

    void replaceInputNode(DFGNode* oldNode, DFGOutput replacement) {
        const auto snapshot = rawInputs();
        for (size_t i = 0; i < snapshot.size(); ++i) {
            if (snapshot[i].node == oldNode) replaceInputAt(i, replacement);
        }
    }

    void setConcatParts(const std::vector<DFGOutput>& parts) {
        payload_ = DFGConcatPayload{parts};
    }

    void appendConcatPart(DFGOutput part) {
        std::get<DFGConcatPayload>(payload_).parts.push_back(part);
    }

    std::string str() const {
        std::string result = to_string(opValue());
        if (!instance_path.empty()) result += "@" + instance_path;
        if (!name.empty()) result += "(" + name + ")";
        if (opValue() == DFGOp::CONST) result += "[" + std::to_string(constValue()) + "]";
        return result;
    }
};

struct DFGTraversal {
    template<typename Fn>
    static void forEachInput(const DFGNode* node, Fn&& fn) {
        const auto& inputs = node->rawInputs();
        for (size_t i = 0; i < inputs.size(); ++i) {
            fn(i, inputs[i]);
        }
    }

    static bool hasInputs(const DFGNode* node) {
        return !node->rawInputs().empty();
    }

};

inline CompilerError::CompilerError(const std::string& msg, const DFGNode* node)
    : std::runtime_error(node && node->loc ? node->loc->str() + ": " + msg : msg),
      loc(node ? node->loc : std::nullopt),
      errorNode(node) {}

} // namespace mate

template<>
struct std::formatter<mate::DFGNode> : std::formatter<std::string> {
    auto format(const mate::DFGNode& node, std::format_context& ctx) const {
        return std::formatter<std::string>::format(node.str(), ctx);
    }
};

template<>
struct std::formatter<mate::DFGNode*> : std::formatter<std::string> {
    auto format(const mate::DFGNode* node, std::format_context& ctx) const {
        if (!node) return std::formatter<std::string>::format("null", ctx);
        return std::formatter<std::string>::format(node->str(), ctx);
    }
};

namespace mate {

struct DFG {
    std::vector<std::unique_ptr<DFGNode>> nodes;

    // Graph-boundary lookup (nullptr if absent)
    DFGNode* getGraphInput(const std::string& instance_path, const std::string& name) const {
        auto it = inputs.find(nodeKey(instance_path, name)); return it != inputs.end() ? it->second : nullptr;
    }
    DFGNode* getGraphOutput(const std::string& instance_path, const std::string& name) const {
        auto it = outputs.find(nodeKey(instance_path, name)); return it != outputs.end() ? it->second : nullptr;
    }
    bool hasGraphInput(const std::string& instance_path, const std::string& name) const {
        return inputs.count(nodeKey(instance_path, name)) > 0;
    }
    bool hasGraphOutput(const std::string& instance_path, const std::string& name) const {
        return outputs.count(nodeKey(instance_path, name)) > 0;
    }
    template<typename Fn>
    void forEachGraphInput(Fn&& fn) const {
        for (const auto& [key, node] : inputs) fn(key, node);
    }
    template<typename Fn>
    void forEachGraphOutput(Fn&& fn) const {
        for (const auto& [key, node] : outputs) fn(key, node);
    }

    const std::map<std::string, DFGNode*>& getConstantsMap() const { return constants; }

    // Create a GraphOutput placeholder node with no driver yet.
    DFGNode* createGraphOutput(const std::string& instance_path, const std::string& name) {
        auto n = std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGOutputPayload{}, name);
        n->instance_path = instance_path;
        nodes.push_back(std::move(n));
        auto result = outputs.insert({nodeKey(instance_path, name), nodes.back().get()});
        if (!result.second) {
            throw CompilerError(std::format("Output {} already exists", name));
        }
        return nodes.back().get();
    }
    // Create an anonymous SIGNAL placeholder node with no driver.
    // Not inserted in named maps, so it is not rooted by DCE.
    DFGNode* placeholderSignal(const std::string& instance_path = "") {
        auto n = std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGSignalPayload{}, "");
        n->instance_path = instance_path;
        nodes.push_back(std::move(n));
        return nodes.back().get();
    }

    // Update all internal map pointers from oldNode to newNode (used by constant_fold / condition_normalization)
    void replaceNodeInMaps(DFGNode* oldNode, DFGNode* newNode) {
        for (auto& [k, v] : constants) if (v == oldNode) v = newNode;
        for (auto& [k, v] : inputs)    if (v == oldNode) v = newNode;
        for (auto& [k, v] : outputs)   if (v == oldNode) v = newNode;
    }

    // Redirect all consumers of oldNode to replacement, and update named maps.
    void redirectConsumers(DFGNode* oldNode, DFGOutput replacement) {
        for (auto& node : nodes) {
            node->replaceInputNode(oldNode, replacement);
        }
        replaceNodeInMaps(oldNode, replacement.node);
    }

    void redirectConsumers(DFGNode* oldNode, DFGNode* newNode) {
        redirectConsumers(oldNode, DFGOutput(newNode));
    }

    // Adopt an existing node into the graph-input map (for DFG inlining).
    // The node must already be in this DFG's nodes vector.
    // Map key is computed from node->instance_path + node->name.
    void adoptGraphInput(DFGNode* node) {
        inputs[nodeKey(node->instance_path, node->name)] = node;
    }

    // Adopt an existing node into the graph-output map (for DFG inlining).
    // The node must already be in this DFG's nodes vector.
    // Map key is computed from node->instance_path + node->name.
    void adoptGraphOutput(DFGNode* node) {
        outputs[nodeKey(node->instance_path, node->name)] = node;
    }

    // Remove map entries whose node is not in the alive set (used by DCE)
    void pruneByAliveSet(const std::unordered_set<DFGNode*>& alive) {
        std::erase_if(constants, [&alive](const auto& kv) { return !alive.count(kv.second); });
        std::erase_if(inputs,    [&alive](const auto& kv) { return !alive.count(kv.second); });
        std::erase_if(outputs,   [&alive](const auto& kv) { return !alive.count(kv.second); });
    }

private:
    static std::string nodeKey(const std::string& ip, const std::string& name) {
        return ip.empty() ? name : ip + "." + name;
    }

    // Quick access named nodes — use accessor methods from outside this class
    std::map<std::string, DFGNode*> constants;
    std::map<std::string, DFGNode*> inputs;
    std::map<std::string, DFGNode*> outputs;

public:

    // INPUT nodes, they have no inputs.

    DFGNode* createGraphInput(const std::string& instance_path, const std::string& name) {
        nodes.push_back(std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGInputPayload{}, name));
        nodes.back()->instance_path = instance_path;
        auto result = inputs.insert({nodeKey(instance_path, name), nodes.back().get()});
        if (!result.second) {
            // Key already exists - insertion failed
            throw CompilerError(std::format("Input {} already exists", name));
        }
        return nodes.back().get();
    }
    DFGNode* named_constant(int64_t v, const std::string& instance_path, const std::string& name) {
        nodes.push_back(std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGConstPayload{v}, name));
        nodes.back()->instance_path = instance_path;
        nodes.back()->type = Type::makeInteger(32, false);
        constants[nodeKey(instance_path, name)] = nodes.back().get();
        return nodes.back().get();
    }

    DFGNode* constant(int64_t v) {
        nodes.push_back(std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGConstPayload{v}));
        nodes.back()->type = Type::makeInteger(32, false);
        return nodes.back().get();
    }

    // Create a named signal placeholder (for internal signals, not ports)
    DFGNode* signal(const std::string& instance_path, const std::string& name) {
        nodes.push_back(std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGSignalPayload{}, name));
        nodes.back()->instance_path = instance_path;
        return nodes.back().get();
    }

    // Create a SLICE node: source[high:low]. high and low MUST be CONST nodes
    // (static bit extraction only). Dynamic indexing must be lowered to MUX instead.
    DFGNode* slice(DFGNode* source, DFGNode* high, DFGNode* low, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGSlicePayload{source, high, low})
            : std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGSlicePayload{source, high, low}, name);
        nodes.push_back(std::move(n));
        return nodes.back().get();
    }

    // Create a CONCAT node: concatenation of parts, MSB-first
    DFGNode* concat(const std::vector<DFGNode*>& parts, const std::string& name = "") {
        std::vector<DFGOutput> inputs;
        inputs.reserve(parts.size());
        for (auto* p : parts) inputs.emplace_back(p);
        return concat(inputs, name);
    }

    DFGNode* concat(const std::vector<DFGOutput>& parts, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGConcatPayload{parts})
            : std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGConcatPayload{parts}, name);
        nodes.push_back(std::move(n));
        return nodes.back().get();
    }

    // Low-level: connect a single driver to an OUTPUT or SIGNAL node.
    void connectDriver(DFGNode* target, DFGOutput driver) {
        if (!target || (target->kind() != DFGOp::OUTPUT && target->kind() != DFGOp::SIGNAL))
            throw CompilerError(std::format(
                "connectDriver: target {} is not OUTPUT or SIGNAL", target ? target->str() : "null"));
        target->setDriver(driver);
    }

    // Connect a driver to an existing graph-output node.
    void connectGraphOutput(const std::string& instance_path, const std::string& name, DFGOutput driver) {
        auto it = outputs.find(nodeKey(instance_path, name));
        if (it == outputs.end()) {
            throw CompilerError(std::format("Output {} not found", name));
        }
        connectDriver(it->second, driver);
    }
    DFGNode* add(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::ADD, a, b, name);
    }

    DFGNode* sub(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::SUB, a, b, name);
    }

    DFGNode* mul(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::MUL, a, b, name);
    }

    DFGNode* eq(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::EQ, a, b, name);
    }

    DFGNode* lt(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::LT, a, b, name);
    }

    DFGNode* le(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::LE, a, b, name);
    }

    DFGNode* gt(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::GT, a, b, name);
    }

    DFGNode* ge(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::GE, a, b, name);
    }

    DFGNode* shl(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::SHL, a, b, name);
    }

    DFGNode* asr(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::ASR, a, b, name);
    }

    DFGNode* mux(DFGNode* sel,
                 const std::vector<int64_t>& selectorValues,
                 const std::vector<DFGNode*>& data,
                 const std::string& name = "") {
        if (selectorValues.size() != data.size()) {
            throw CompilerError("mux: selectorValues and data must have same size");
        }
        if (selectorValues.size() < 2) {
            throw CompilerError("mux: must have at least two data arms");
        }
        std::vector<DFGMuxArm> arms;
        arms.reserve(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            arms.push_back({selectorValues[i], DFGOutput(data[i])});
        }
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGMuxPayload{DFGOutput(sel), std::move(arms)})
            : std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGMuxPayload{DFGOutput(sel), std::move(arms)}, name);
        nodes.push_back(std::move(n));
        return nodes.back().get();
    }

    DFGNode* mux(DFGNode* sel, DFGNode* t, DFGNode* f, const std::string& name = "") {
        return mux(sel, {1, 0}, {t, f}, name);
    }

    // Helper for unary operations (single input)
    DFGNode* binaryOp(DFGOp op, DFGNode* a, DFGNode* b, const std::string& name = "") {
        if (!isBinaryDFGOp(op))
            throw CompilerError(std::format("binaryOp: {} is not a binary op", to_string(op)));
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGBinaryPayload{op, DFGOutput(a), DFGOutput(b)})
            : std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGBinaryPayload{op, DFGOutput(a), DFGOutput(b)}, name);
        nodes.push_back(std::move(n));
        return nodes.back().get();
    }

    DFGNode* unaryOp(DFGOp op, DFGNode* a, const std::string& name = "") {
        if (!isUnaryDFGOp(op))
            throw CompilerError(std::format("unaryOp: {} is not a unary op", to_string(op)));
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGUnaryPayload{op, DFGOutput(a)})
            : std::make_unique<DFGNode>(DFGNode::ConstructionKey{}, DFGUnaryPayload{op, DFGOutput(a)}, name);
        nodes.push_back(std::move(n));
        return nodes.back().get();
    }

    DFGNode* unaryNegate(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::UNARY_NEGATE, a, name);
    }

    DFGNode* bitwiseNot(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::BITWISE_NOT, a, name);
    }

    DFGNode* bitwiseAnd(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::BITWISE_AND, a, b, name);
    }
    DFGNode* bitwiseOr(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::BITWISE_OR, a, b, name);
    }
    DFGNode* bitwiseXor(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::BITWISE_XOR, a, b, name);
    }
    DFGNode* bitwiseXnor(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::BITWISE_XNOR, a, b, name);
    }

    DFGNode* reductionAnd(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::REDUCTION_AND, a, name);
    }

    DFGNode* reductionNand(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::REDUCTION_NAND, a, name);
    }

    DFGNode* reductionOr(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::REDUCTION_OR, a, name);
    }

    DFGNode* reductionNor(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::REDUCTION_NOR, a, name);
    }

    DFGNode* reductionXor(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::REDUCTION_XOR, a, name);
    }

    DFGNode* reductionXnor(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::REDUCTION_XNOR, a, name);
    }

    // Validate that every node with inputs has the correct count for its op.
    // Nodes with 0 inputs are always allowed (orphans cleaned up by DCE).
    void validate() const {
        for (const auto& node : nodes) {
            int expected = expectedInputs(node->kind());
            int actual = static_cast<int>(node->rawInputs().size());

            if (actual == 0) {
                if (node->kind() != DFGOp::INPUT &&
                    node->kind() != DFGOp::CONST &&
                    node->kind() != DFGOp::OUTPUT &&
                    node->kind() != DFGOp::SIGNAL) {
                    throw CompilerError(std::format(
                        "DFG validate: {} has no inputs but is not INPUT, CONST, OUTPUT, or SIGNAL",
                        node->str()), node.get());
                }
                continue;
            }

            if (expected >= 0 && actual != expected) {
                throw CompilerError(std::format(
                    "DFG validate: {} expects {} inputs, has {}",
                    node->str(), expected, actual), node.get());
            }

            // All DFGOutput entries must have non-null node and valid port
            for (const auto& input : node->rawInputs()) {
                if (!input.node)
                    throw CompilerError(std::format(
                        "DFG validate: node {} has null input node", node->str()), node.get());
                if (input.port < 0 || input.port >= input.node->num_outputs())
                    throw CompilerError(std::format(
                        "DFG validate: node {} references port {} of {} (has {} outputs)",
                        node->str(), input.port, input.node->str(),
                        input.node->num_outputs()), node.get());
            }

            // SIGNAL has at most one driver
            if (node->kind() == DFGOp::SIGNAL && actual > 1)
                throw CompilerError(std::format(
                    "DFG validate: SIGNAL {} has {} inputs (expected 0 or 1)",
                    node->name, actual), node.get());

            // Variable-count ops with specific constraints
            if (node->kind() == DFGOp::OUTPUT && actual > 1) {
                throw CompilerError(std::format(
                    "DFG validate: OUTPUT {} has {} inputs (expected 0 or 1)",
                    node->name, actual), node.get());
            }
            if (node->kind() == DFGOp::MUX) {
                if (actual < 3) {
                    throw CompilerError(std::format(
                        "DFG validate: MUX {} has {} inputs (expected selector + at least 2 arms)",
                        node->str(), actual), node.get());
                }
                const auto& muxValues = node->muxValues();
                if (muxValues.size() + 1 != node->rawInputs().size()) {
                    throw CompilerError(std::format(
                        "DFG validate: MUX {} has {} arms but {} mux_values",
                        node->str(), actual - 1, muxValues.size()), node.get());
                }
                std::set<int64_t> seenValues(muxValues.begin(), muxValues.end());
                if (seenValues.size() != muxValues.size()) {
                    throw CompilerError(std::format(
                        "DFG validate: MUX {} has duplicate selector values", node->str()), node.get());
                }
                auto* selector = node->muxSelector().node;
                if (!selector || !selector->hasType()) {
                    continue;
                }
                int width = selector->type->width;
                if (width <= 0 || width >= 63) {
                    throw CompilerError(std::format(
                        "DFG validate: MUX {} has unsupported selector width {}",
                        node->str(), width), node.get());
                }
                int64_t expectedArms = int64_t(1) << width;
                if (static_cast<int64_t>(muxValues.size()) != expectedArms) {
                    throw CompilerError(std::format(
                        "DFG validate: MUX {} has {} arms for selector width {} (expected {})",
                        node->str(), muxValues.size(), width, expectedArms), node.get());
                }
                for (int64_t value = 0; value < expectedArms; ++value) {
                    if (!seenValues.contains(value)) {
                        throw CompilerError(std::format(
                            "DFG validate: MUX {} is missing selector value {}",
                            node->str(), value), node.get());
                    }
                }
            }
        }
    }

    // Call after DCE. Rejects shapes that must not survive to the live DFG:
    // - any non-INPUT/CONST node with zero inputs (undriven or tombstoned)
    // - any node still carrying unpacked_dims (arrays must be leaf-bound)
    void validateStrictLiveDFG() const {
        for (const auto& node : nodes) {
            if (node->rawInputs().empty() &&
                node->kind() != DFGOp::INPUT &&
                node->kind() != DFGOp::CONST) {
                throw CompilerError(std::format(
                    "DFG validateStrict: node {} has no inputs but is not INPUT or CONST",
                    node->str()), node.get());
            }
            if (node->type.has_value() && !node->type->unpacked_dims.empty()) {
                throw CompilerError(std::format(
                    "DFG validateStrict: node {} still carries unpacked_dims",
                    node->str()), node.get());
            }
        }
    }

    // Check that no node that requires inputs has 0.
    // Run after DCE — orphan nodes should have been removed by then.
    void validateNoOrphans() const {
        for (const auto& node : nodes) {
            int expected = expectedInputs(node->kind());
            int actual = static_cast<int>(node->rawInputs().size());
            if (expected > 0 && actual == 0) {
                throw CompilerError(std::format(
                    "DFG validateNoOrphans: {} expects {} inputs, has 0",
                    node->str(), expected), node.get());
            }
        }
    }

    std::string toDot(const std::string& graphName = "DFG",
                      const std::set<const DFGNode*>& errorNodes = {}) const;
    std::string toDotCone(const DFGNode* root,
                          const std::string& graphName = "DFG") const;
    std::string toJson(int indent = 0) const;
    std::string toJsonCone(const DFGNode* root, int indent = 0) const;

private:
    std::string renderDot(const std::string& graphName,
                          const std::set<const DFGNode*>& errorNodes,
                          const std::set<const DFGNode*>* filter) const;
    std::string renderJson(int indent,
                           const std::set<const DFGNode*>* filter) const;
};

} // namespace mate
