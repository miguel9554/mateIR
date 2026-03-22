#pragma once

#include "ir/types.h"
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

namespace custom_hdl {

enum class DFGOp {
    INPUT,      // Primary input (module port)
    OUTPUT,     // Primary output (module port)
    SIGNAL,     // Internal signal (named placeholder)
    CONST,      // Constant value (data: int64_t)
    INDEX,      // Indexing: in[0]=source, in[1]=high, in[2]=low
    CONCAT,       // Concatenation: in[0..N-1] = parts, MSB-first
    CONCAT_ALIGN, // Temporary: in[0]=expr, in[1]=high_idx, in[2]=low_idx
    // Binary ops
    ADD,
    SUB,
    MUL,
    DIV,
    EQ,         // Equal (==)
    LT,         // Less than (<)
    LE,         // Less or equal (<=)
    GT,         // Greater than (>)
    GE,         // Greater or equal (>=)
    SHL,        // Arithmetic shift left (<<<)
    ASR,        // Arithmetic shift right (>>>)
    POWER,      // Exponentiation (**)
    MUX,        // 2:1 mux: in[0]=sel, in[1]=true_val, in[2]=false_val
    MUX_N,      // N:1 mux: in[0..N-1]=selectors, in[N..2N-1]=data values (one-hot select)
    MODULE,     // Submodule instance: name=instance_name, data=module_type_name, in=input_port_drivers
    // Unary ops (single input)
    UNARY_PLUS,
    UNARY_NEGATE,
    BITWISE_NOT,
    LOGICAL_NOT,
    // Binary bitwise/logical ops
    LOGICAL_AND,
    LOGICAL_OR,
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
        case DFGOp::INDEX: return "INDEX";
        case DFGOp::CONCAT: return "CONCAT";
        case DFGOp::CONCAT_ALIGN: return "CONCAT_ALIGN";
        case DFGOp::ADD: return "ADD";
        case DFGOp::SUB: return "SUB";
        case DFGOp::MUL: return "MUL";
        case DFGOp::DIV: return "DIV";
        case DFGOp::EQ: return "EQ";
        case DFGOp::LT: return "LT";
        case DFGOp::LE: return "LE";
        case DFGOp::GT: return "GT";
        case DFGOp::GE: return "GE";
        case DFGOp::SHL: return "SHL";
        case DFGOp::ASR: return "ASR";
        case DFGOp::POWER: return "POWER";
        case DFGOp::MUX: return "MUX";
        case DFGOp::MUX_N: return "MUX_N";
        case DFGOp::MODULE: return "MODULE";
        case DFGOp::UNARY_PLUS: return "UNARY_PLUS";
        case DFGOp::UNARY_NEGATE: return "UNARY_NEGATE";
        case DFGOp::BITWISE_NOT: return "BITWISE_NOT";
        case DFGOp::LOGICAL_NOT: return "LOGICAL_NOT";
        case DFGOp::LOGICAL_AND:  return "LOGICAL_AND";
        case DFGOp::LOGICAL_OR:   return "LOGICAL_OR";
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
        // SIGNAL: 0 (undriven), 1 (driven), or N (aggregate) — validated separately
        case DFGOp::SIGNAL: return -1;
        // MODULE: variable input count (depends on ports)
        case DFGOp::MODULE: return -1;
        // MUX_N: variable (even, >= 2) — validated separately
        case DFGOp::MUX_N:  return -1;

        case DFGOp::INDEX:  return 3;
        case DFGOp::CONCAT: return -1; // variable
        case DFGOp::CONCAT_ALIGN: return 3;
        case DFGOp::ADD:    return 2;
        case DFGOp::SUB:    return 2;
        case DFGOp::MUL:    return 2;
        case DFGOp::DIV:    return 2;
        case DFGOp::EQ:     return 2;
        case DFGOp::LT:     return 2;
        case DFGOp::LE:     return 2;
        case DFGOp::GT:     return 2;
        case DFGOp::GE:     return 2;
        case DFGOp::SHL:    return 2;
        case DFGOp::ASR:    return 2;
        case DFGOp::POWER:  return 2;
        case DFGOp::MUX:    return 3;

        case DFGOp::UNARY_PLUS:      return 1;
        case DFGOp::UNARY_NEGATE:    return 1;
        case DFGOp::BITWISE_NOT:     return 1;
        case DFGOp::LOGICAL_NOT:     return 1;
        case DFGOp::LOGICAL_AND:  return 2;
        case DFGOp::LOGICAL_OR:   return 2;
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

} // namespace custom_hdl

template<>
struct std::formatter<custom_hdl::DFGOp> : std::formatter<const char*> {
    auto format(custom_hdl::DFGOp op, std::format_context& ctx) const {
        return std::formatter<const char*>::format(custom_hdl::to_string(op), ctx);
    }
};

namespace custom_hdl {

struct DFGNode; // forward declare

struct DFGOutput {
    DFGNode* node;
    int port = 0;
    DFGOutput(DFGNode* n, int p = 0) : node(n), port(p) {}
};

struct DFGNode {
    DFGOp op;
    std::vector<DFGOutput> in;  // fan-in (operands)

    std::string name;  // Optional name (empty = anonymous)

    // Hierarchical path set during DFG inlining (empty = top-level node).
    // For inlined sub-module nodes: "inst" or "inst.subinst"
    std::string instance_path;

    // Metadata depending on op type:
    // - monostate: no extra data
    // - int64_t: constant value (CONST)
    std::variant<
        std::monostate,
        int64_t,
        std::string
    > data;

    // Type info (width, signedness) — set on leaf nodes, propagated by type pass
    std::optional<ResolvedType> type;

    // Source location in the original Verilog (set during elaboration)
    std::optional<SourceLoc> loc;

    // Multi-output support: empty = single unnamed output
    std::vector<std::string> output_names;

    // Input port names (MODULE nodes only): parallel to in vector
    std::vector<std::string> input_names;

    bool hasType() const { return type.has_value(); }

    int num_outputs() const {
        return output_names.empty() ? 1 : static_cast<int>(output_names.size());
    }

    int output_index(const std::string& oname) const {
        for (int i = 0; i < static_cast<int>(output_names.size()); i++)
            if (output_names[i] == oname) return i;
        return -1;
    }

    int input_index(const std::string& iname) const {
        for (int i = 0; i < static_cast<int>(input_names.size()); i++)
            if (input_names[i] == iname) return i;
        return -1;
    }

    DFGOutput output(int port = 0) { return {this, port}; }
    DFGOutput output(const std::string& oname) { return {this, output_index(oname)}; }

    DFGNode(DFGOp o) : op(o) {}
    DFGNode(DFGOp o, std::string name) : op(o), name(std::move(name)) {}
    DFGNode(DFGOp o, int64_t val) : op(o), data(val) {}
    DFGNode(DFGOp o, std::string name, int64_t val) : op(o), name(std::move(name)), data(val) {}
    DFGNode(DFGOp o, std::string name, std::string str_data) : op(o), name(std::move(name)), data(std::move(str_data)) {}

    std::string str() const {
        std::string result = to_string(op);
        if (!instance_path.empty()) result += "@" + instance_path;
        if (!name.empty()) result += "(" + name + ")";
        if (std::holds_alternative<int64_t>(data)) result += "[" + std::to_string(std::get<int64_t>(data)) + "]";
        if (std::holds_alternative<std::string>(data)) result += "{" + std::get<std::string>(data) + "}";
        return result;
    }
};

inline CompilerError::CompilerError(const std::string& msg, const DFGNode* node)
    : std::runtime_error(node && node->loc ? node->loc->str() + ": " + msg : msg),
      loc(node ? node->loc : std::nullopt),
      errorNode(node) {}

} // namespace custom_hdl

template<>
struct std::formatter<custom_hdl::DFGNode> : std::formatter<std::string> {
    auto format(const custom_hdl::DFGNode& node, std::format_context& ctx) const {
        return std::formatter<std::string>::format(node.str(), ctx);
    }
};

template<>
struct std::formatter<custom_hdl::DFGNode*> : std::formatter<std::string> {
    auto format(const custom_hdl::DFGNode* node, std::format_context& ctx) const {
        if (!node) return std::formatter<std::string>::format("null", ctx);
        return std::formatter<std::string>::format(node->str(), ctx);
    }
};

namespace custom_hdl {

struct DFG {
    std::vector<std::unique_ptr<DFGNode>> nodes;

    // Named-node lookup (nullptr if absent)
    DFGNode* getInputNode(const std::string& instance_path, const std::string& name) const {
        auto it = inputs.find(nodeKey(instance_path, name)); return it != inputs.end() ? it->second : nullptr;
    }
    DFGNode* getOutputNode(const std::string& instance_path, const std::string& name) const {
        auto it = outputs.find(nodeKey(instance_path, name)); return it != outputs.end() ? it->second : nullptr;
    }
    DFGNode* getSignalNode(const std::string& instance_path, const std::string& name) const {
        auto it = signals.find(nodeKey(instance_path, name)); return it != signals.end() ? it->second : nullptr;
    }
    bool hasOutput(const std::string& instance_path, const std::string& name) const { return outputs.count(nodeKey(instance_path, name)) > 0; }
    bool hasSignal(const std::string& instance_path, const std::string& name) const { return signals.count(nodeKey(instance_path, name)) > 0; }

    // Read-only iteration for passes
    const std::map<std::string, DFGNode*>& getInputsMap()    const { return inputs; }
    const std::map<std::string, DFGNode*>& getOutputsMap()   const { return outputs; }
    const std::map<std::string, DFGNode*>& getSignalsMap()   const { return signals; }
    const std::map<std::string, DFGNode*>& getConstantsMap() const { return constants; }

    // Create an OUTPUT placeholder node with no driver yet
    DFGNode* outputPlaceholder(const std::string& instance_path, const std::string& name) {
        auto n = std::make_unique<DFGNode>(DFGOp::OUTPUT, name);
        n->instance_path = instance_path;
        nodes.push_back(std::move(n));
        auto result = outputs.insert({nodeKey(instance_path, name), nodes.back().get()});
        if (!result.second) {
            throw CompilerError(std::format("Output {} already exists", name));
        }
        return nodes.back().get();
    }

    // Update all internal map pointers from oldNode to newNode (used by constant_fold / condition_normalization)
    void replaceNodeInMaps(DFGNode* oldNode, DFGNode* newNode) {
        for (auto& [k, v] : constants) if (v == oldNode) v = newNode;
        for (auto& [k, v] : inputs)    if (v == oldNode) v = newNode;
        for (auto& [k, v] : outputs)   if (v == oldNode) v = newNode;
        for (auto& [k, v] : signals)   if (v == oldNode) v = newNode;
    }

    // Adopt an existing node into the inputs map (for DFG inlining).
    // The node must already be in this DFG's nodes vector.
    // Map key is computed from node->instance_path + node->name.
    void adoptInput(DFGNode* node) {
        inputs[nodeKey(node->instance_path, node->name)] = node;
    }

    // Adopt an existing node into the outputs map (for DFG inlining).
    // The node must already be in this DFG's nodes vector.
    // Map key is computed from node->instance_path + node->name.
    void adoptOutput(DFGNode* node) {
        outputs[nodeKey(node->instance_path, node->name)] = node;
    }

    // Remove map entries whose node is not in the alive set (used by DCE)
    void pruneByAliveSet(const std::unordered_set<DFGNode*>& alive) {
        std::erase_if(constants, [&alive](const auto& kv) { return !alive.count(kv.second); });
        std::erase_if(inputs,    [&alive](const auto& kv) { return !alive.count(kv.second); });
        std::erase_if(outputs,   [&alive](const auto& kv) { return !alive.count(kv.second); });
        std::erase_if(signals,   [&alive](const auto& kv) { return !alive.count(kv.second); });
    }

private:
    static std::string nodeKey(const std::string& ip, const std::string& name) {
        return ip.empty() ? name : ip + "." + name;
    }

    // Quick access named nodes — use accessor methods from outside this class
    std::map<std::string, DFGNode*> constants;
    std::map<std::string, DFGNode*> inputs;
    std::map<std::string, DFGNode*> outputs;
    std::map<std::string, DFGNode*> signals;

public:

    // INPUT nodes, they have no inputs.

    DFGNode* input(const std::string& instance_path, const std::string& name) {
        nodes.push_back(std::make_unique<DFGNode>(DFGOp::INPUT, name));
        nodes.back()->instance_path = instance_path;
        auto result = inputs.insert({nodeKey(instance_path, name), nodes.back().get()});
        if (!result.second) {
            // Key already exists - insertion failed
            throw CompilerError(std::format("Input {} already exists", name));
        }
        return nodes.back().get();
    }

    DFGNode* named_constant(int64_t v, const std::string& instance_path, const std::string& name) {
        nodes.push_back(std::make_unique<DFGNode>(DFGOp::CONST, name, v));
        nodes.back()->instance_path = instance_path;
        nodes.back()->type = ResolvedType::makeInteger(32, false);
        constants[nodeKey(instance_path, name)] = nodes.back().get();
        return nodes.back().get();
    }

    DFGNode* constant(int64_t v) {
        nodes.push_back(std::make_unique<DFGNode>(DFGOp::CONST, v));
        nodes.back()->type = ResolvedType::makeInteger(32, false);
        return nodes.back().get();
    }

    // Create a named signal placeholder (for internal signals, not ports)
    DFGNode* signal(const std::string& instance_path, const std::string& name) {
        nodes.push_back(std::make_unique<DFGNode>(DFGOp::SIGNAL, name));
        nodes.back()->instance_path = instance_path;
        auto result = signals.insert({nodeKey(instance_path, name), nodes.back().get()});
        if (!result.second) {
            throw CompilerError(std::format("Signal {} already exists", name));
        }
        return nodes.back().get();
    }

    // Create an INDEX node: source[high:low] (single-element: high == low)
    DFGNode* index(DFGNode* source, DFGNode* high, DFGNode* low, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::INDEX)
            : std::make_unique<DFGNode>(DFGOp::INDEX, name);
        n->in = {source, high, low};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    // Create a CONCAT node: concatenation of parts, MSB-first
    DFGNode* concat(const std::vector<DFGNode*>& parts, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::CONCAT)
            : std::make_unique<DFGNode>(DFGOp::CONCAT, name);
        for (auto* p : parts) n->in.push_back(p);
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    // Create a CONCAT_ALIGN node: temporary wrapper with positional metadata
    DFGNode* concatAlign(DFGNode* expr, DFGNode* high, DFGNode* low, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::CONCAT_ALIGN)
            : std::make_unique<DFGNode>(DFGOp::CONCAT_ALIGN, name);
        n->in = {expr, high, low};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    // Lookup signal for READING in expressions
    // Returns the node representing the signal's value
    DFGNode* lookupSignal(const std::string& instance_path, const std::string& name) const {
        std::string key = nodeKey(instance_path, name);
        if (auto it = inputs.find(key); it != inputs.end()) return it->second;
        if (auto it = signals.find(key); it != signals.end()) return it->second;
        if (auto it = constants.find(key); it != constants.end()) return it->second;
        if (auto it = outputs.find(key); it != outputs.end()) {
            // For outputs, return the driver if connected (reading output value)
            auto* outNode = it->second;
            return outNode->in.empty() ? outNode : outNode->in[0].node;
        }
        return nullptr;
    }

    // Connect a driver to an existing output node
    void connectOutput(const std::string& instance_path, const std::string& name, DFGOutput driver) {
        auto it = outputs.find(nodeKey(instance_path, name));
        if (it == outputs.end()) {
            throw CompilerError(std::format("Output {} not found", name));
        }
        it->second->in = {driver};
    }

    // Connect a driver to an existing signal node
    void connectSignal(const std::string& instance_path, const std::string& name, DFGOutput driver) {
        auto it = signals.find(nodeKey(instance_path, name));
        if (it == signals.end()) {
            throw CompilerError(std::format("Signal {} not found", name));
        }
        it->second->in = {driver};
    }

    // An output can be recreated, if it's assigned again.
    DFGNode* output(DFGNode* a, const std::string& instance_path, const std::string& name, bool rename) {
        auto n = std::make_unique<DFGNode>(DFGOp::OUTPUT, name);
        n->instance_path = instance_path;
        n->in = {a};
        nodes.push_back(std::move(n));
        std::string key = nodeKey(instance_path, name);
        auto result = outputs.insert({key, nodes.back().get()});
        if (!result.second) {
            if (!rename) throw CompilerError(std::format("Output {} already exists", name));
            // Remove old output node from the nodes vector
            auto oldNode = outputs[key];
            auto it = std::find_if(nodes.begin(), nodes.end(),
                [oldNode](const std::unique_ptr<DFGNode>& n) {
                    return n.get() == oldNode;
                });
            if (it != nodes.end()) {
                nodes.erase(it);
            }
            outputs[key] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* add(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::ADD)
            : std::make_unique<DFGNode>(DFGOp::ADD, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* sub(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::SUB)
            : std::make_unique<DFGNode>(DFGOp::SUB, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* mul(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::MUL)
            : std::make_unique<DFGNode>(DFGOp::MUL, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* eq(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::EQ)
            : std::make_unique<DFGNode>(DFGOp::EQ, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* lt(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::LT)
            : std::make_unique<DFGNode>(DFGOp::LT, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* le(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::LE)
            : std::make_unique<DFGNode>(DFGOp::LE, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* gt(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::GT)
            : std::make_unique<DFGNode>(DFGOp::GT, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* ge(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::GE)
            : std::make_unique<DFGNode>(DFGOp::GE, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* shl(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::SHL)
            : std::make_unique<DFGNode>(DFGOp::SHL, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* asr(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::ASR)
            : std::make_unique<DFGNode>(DFGOp::ASR, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* power(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::POWER)
            : std::make_unique<DFGNode>(DFGOp::POWER, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* mux(DFGNode* sel, DFGNode* t, DFGNode* f, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::MUX)
            : std::make_unique<DFGNode>(DFGOp::MUX, name);
        n->in = {sel, t, f};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    // N:1 mux with one-hot selectors
    // selectors[i] active => output = data[i]
    DFGNode* muxN(const std::vector<DFGNode*>& selectors,
                  const std::vector<DFGNode*>& data,
                  const std::string& name = "") {
        if (selectors.size() != data.size()) {
            throw CompilerError("muxN: selectors and data must have same size");
        }
        if (selectors.empty()) {
            throw CompilerError("muxN: must have at least one input");
        }
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::MUX_N)
            : std::make_unique<DFGNode>(DFGOp::MUX_N, name);
        // Layout: selectors first, then data
        n->in.reserve(selectors.size() + data.size());
        for (auto* s : selectors) n->in.push_back(s);
        for (auto* d : data) n->in.push_back(d);
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    // Create a MODULE instance node
    // moduleName = the type of module being instantiated
    // instanceName = the name of this instance
    // in = drivers for each input port (added after creation)
    DFGNode* module(const std::string& moduleName, const std::string& instanceName,
                    const std::vector<std::string>& outputPortNames = {}) {
        nodes.push_back(std::make_unique<DFGNode>(DFGOp::MODULE, instanceName, moduleName));
        nodes.back()->output_names = outputPortNames;
        return nodes.back().get();
    }

    // Helper for unary operations (single input)
    DFGNode* binaryOp(DFGOp op, DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(op)
            : std::make_unique<DFGNode>(op, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) signals[name] = nodes.back().get();
        return nodes.back().get();
    }

    DFGNode* unaryOp(DFGOp op, DFGNode* a, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(op)
            : std::make_unique<DFGNode>(op, name);
        n->in = {a};
        nodes.push_back(std::move(n));
        if (!name.empty()) {
            signals[name] = nodes.back().get();
        }
        return nodes.back().get();
    }

    DFGNode* unaryPlus(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::UNARY_PLUS, a, name);
    }

    DFGNode* unaryNegate(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::UNARY_NEGATE, a, name);
    }

    DFGNode* bitwiseNot(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::BITWISE_NOT, a, name);
    }

    DFGNode* logicalNot(DFGNode* a, const std::string& name = "") {
        return unaryOp(DFGOp::LOGICAL_NOT, a, name);
    }

    DFGNode* logicalAnd(DFGNode* a, DFGNode* b, const std::string& name = "") {
        auto n = name.empty()
            ? std::make_unique<DFGNode>(DFGOp::LOGICAL_AND)
            : std::make_unique<DFGNode>(DFGOp::LOGICAL_AND, name);
        n->in = {a, b};
        nodes.push_back(std::move(n));
        if (!name.empty()) signals[name] = nodes.back().get();
        return nodes.back().get();
    }

    DFGNode* logicalOr(DFGNode* a, DFGNode* b, const std::string& name = "") {
        return binaryOp(DFGOp::LOGICAL_OR, a, b, name);
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
            int expected = expectedInputs(node->op);
            int actual = static_cast<int>(node->in.size());

            if (actual == 0) continue; // orphan — valid until DCE

            if (expected >= 0 && actual != expected) {
                throw CompilerError(std::format(
                    "DFG validate: {} expects {} inputs, has {}",
                    node->str(), expected, actual), node.get());
            }

            // Variable-count ops with specific constraints
            if (node->op == DFGOp::OUTPUT && actual > 1) {
                throw CompilerError(std::format(
                    "DFG validate: OUTPUT {} has {} inputs (expected 0 or 1)",
                    node->name, actual), node.get());
            }
            if (node->op == DFGOp::MUX_N) {
                if (actual < 2 || actual % 2 != 0) {
                    throw CompilerError(std::format(
                        "DFG validate: MUX_N {} has {} inputs (expected even, >= 2)",
                        node->str(), actual), node.get());
                }
            }
        }
    }

    // Check that no node that requires inputs has 0.
    // Run after DCE — orphan nodes should have been removed by then.
    void validateNoOrphans() const {
        for (const auto& node : nodes) {
            int expected = expectedInputs(node->op);
            int actual = static_cast<int>(node->in.size());
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

} // namespace custom_hdl
