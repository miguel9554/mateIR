#include "ir/dfg.h"

#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace custom_hdl {

std::string DFG::renderDot(const std::string& graphName,
                           const std::set<const DFGNode*>& errorNodes,
                           const std::set<const DFGNode*>* filter) const {
    std::ostringstream ss;
    // Sanitize graph name: replace characters invalid in DOT identifiers
    std::string safeName = graphName;
    for (auto& c : safeName) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            c = '_';
        }
    }
    ss << "digraph " << safeName << " {\n";
    ss << "  rankdir=LR;\n";
    ss << "  splines=polyline;\n";
    ss << "  node [shape=box];\n\n";

    // Build node index map
    std::map<const DFGNode*, size_t> nodeIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeIndex[nodes[i].get()] = i;
    }

    // Output nodes
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (filter && !filter->count(node.get())) continue;

        ss << "  n" << i << " [label=\"";

        switch (node->op) {
            case DFGOp::INPUT:
                ss << "INPUT\\n" << node->name;
                break;
            case DFGOp::OUTPUT:
                ss << "OUTPUT\\n" << node->name;
                break;
            case DFGOp::SIGNAL:
                ss << "SIGNAL\\n" << node->name;
                break;
            case DFGOp::CONST:
                ss << "CONST\\n";
                if (!node->name.empty()) ss << node->name << "\\n";
                ss << std::get<int64_t>(node->data);
                break;
            case DFGOp::ADD: ss << "+"; break;
            case DFGOp::SUB: ss << "-"; break;
            case DFGOp::MUL: ss << "*"; break;
            case DFGOp::DIV: ss << "/"; break;
            case DFGOp::EQ:  ss << "=="; break;
            case DFGOp::LT:  ss << "<"; break;
            case DFGOp::LE:  ss << "<="; break;
            case DFGOp::GT:  ss << ">"; break;
            case DFGOp::GE:  ss << ">="; break;
            case DFGOp::SHL: ss << "<<<"; break;
            case DFGOp::ASR: ss << ">>>"; break;
            case DFGOp::POWER: ss << "**"; break;
            case DFGOp::MUX: ss << "MUX"; break;
            case DFGOp::MUX_N: ss << "MUX_N"; break;
            case DFGOp::MODULE:
                ss << "MODULE\\n" << node->name;
                if (std::holds_alternative<std::string>(node->data))
                    ss << "\\n(" << std::get<std::string>(node->data) << ")";
                if (!node->output_names.empty()) {
                    ss << "\\nouts: ";
                    for (size_t oi = 0; oi < node->output_names.size(); ++oi) {
                        if (oi > 0) ss << ", ";
                        ss << node->output_names[oi];
                    }
                }
                break;
            case DFGOp::INDEX: ss << "INDEX"; break;
            case DFGOp::CONCAT: ss << "CONCAT"; break;
            case DFGOp::CONCAT_ALIGN: ss << "CONCAT_ALIGN"; break;
            case DFGOp::UNARY_PLUS: ss << "+"; break;
            case DFGOp::UNARY_NEGATE: ss << "-"; break;
            case DFGOp::BITWISE_NOT: ss << "~"; break;
            case DFGOp::LOGICAL_NOT: ss << "!"; break;
            case DFGOp::LOGICAL_AND:  ss << "&&"; break;
            case DFGOp::LOGICAL_OR:   ss << "||"; break;
            case DFGOp::BITWISE_AND:  ss << "&"; break;
            case DFGOp::BITWISE_OR:   ss << "|"; break;
            case DFGOp::BITWISE_XOR:  ss << "^"; break;
            case DFGOp::BITWISE_XNOR: ss << "~^"; break;
            case DFGOp::REDUCTION_AND: ss << "&"; break;
            case DFGOp::REDUCTION_NAND: ss << "~&"; break;
            case DFGOp::REDUCTION_OR: ss << "|"; break;
            case DFGOp::REDUCTION_NOR: ss << "~|"; break;
            case DFGOp::REDUCTION_XOR: ss << "^"; break;
            case DFGOp::REDUCTION_XNOR: ss << "~^"; break;
        }

        // Append type info if available
        if (node->hasType()) {
            ss << "\\n[";
            for (const auto& dim : node->type->unpacked_dims) {
                ss << dim.size() << "x";
            }
            ss << node->type->width;
            ss << (node->type->isSigned() ? "s" : "u") << "]";
        }
        if (node->loc) {
            ss << "\\n" << node->loc->file << ":" << node->loc->line;
        }
        ss << "\"";
        if (errorNodes.count(node.get())) {
            ss << ", style=filled, fillcolor=red, fontcolor=white";
        }
        ss << "];\n";
    }

    ss << "\n";

    // Returns the label for the j-th input edge of a MUX/MUX_N node
    auto inputLabel = [](DFGOp op, size_t j, size_t totalInputs) -> std::string {
        switch (op) {
            case DFGOp::MUX:
                if (j == 0) return "sel";
                if (j == 1) return "T";
                if (j == 2) return "F";
                break;
            case DFGOp::INDEX:
                if (j == 0) return "src";
                if (j == 1) return "hi";
                if (j == 2) return "lo";
                break;
            case DFGOp::CONCAT_ALIGN:
                if (j == 0) return "expr";
                if (j == 1) return "hi";
                if (j == 2) return "lo";
                break;
            case DFGOp::MUX_N: {
                size_t nSel = totalInputs / 2;
                if (j < nSel) return "sel" + std::to_string(j);
                return "d" + std::to_string(j - nSel);
            }
            default:
                break;
        }
        return "";
    };

    // Output edges
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (filter && !filter->count(node.get())) continue;

        for (size_t j = 0; j < node->in.size(); ++j) {
            const auto& input = node->in[j];
            if (filter && !filter->count(input.node)) continue;
            ss << "  n" << nodeIndex.at(input.node) << " -> n" << i;
            std::string label = inputLabel(node->op, j, node->in.size());
            if (input.port != 0) {
                label = label.empty()
                    ? "port " + std::to_string(input.port)
                    : label + " (port " + std::to_string(input.port) + ")";
            }
            if (!label.empty()) {
                ss << " [label=\"" << label << "\"]";
            }
            ss << ";\n";
        }
    }

    ss << "}\n";
    return ss.str();
}

std::string DFG::toDot(const std::string& graphName,
                       const std::set<const DFGNode*>& errorNodes) const {
    return renderDot(graphName, errorNodes, nullptr);
}

std::string DFG::toDotCone(const DFGNode* root,
                           const std::string& graphName) const {
    // BFS backward through fanin edges to collect the cone
    std::set<const DFGNode*> cone;
    std::queue<const DFGNode*> q;
    cone.insert(root);
    q.push(root);
    while (!q.empty()) {
        const DFGNode* curr = q.front();
        q.pop();
        for (const auto& input : curr->in) {
            if (cone.insert(input.node).second) {
                q.push(input.node);
            }
        }
    }
    return renderDot(graphName, {}, &cone);
}

std::string DFG::renderJson(int indent, const std::set<const DFGNode*>* filter) const {
    auto indentStr = [](int n) { return std::string(n * 2, ' '); };

    // Build node index map
    std::map<const DFGNode*, size_t> nodeIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        nodeIndex[nodes[i].get()] = i;
    }

    std::ostringstream ss;
    ss << indentStr(indent) << "{\n";
    ss << indentStr(indent + 1) << "\"nodes\": [\n";

    bool firstNode = true;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (filter && !filter->count(node.get())) continue;

        if (!firstNode) ss << ",\n";
        firstNode = false;

        ss << indentStr(indent + 2) << "{\n";
        ss << indentStr(indent + 3) << "\"id\": " << i << ",\n";
        ss << indentStr(indent + 3) << "\"op\": \"" << to_string(node->op) << "\",\n";

        // Add name if present
        if (!node->name.empty()) {
            ss << indentStr(indent + 3) << "\"name\": \"" << node->name << "\",\n";
        }
        // Add data field based on variant type
        if (std::holds_alternative<int64_t>(node->data)) {
            ss << indentStr(indent + 3) << "\"value\": " << std::get<int64_t>(node->data) << ",\n";
        }
        if (std::holds_alternative<std::string>(node->data)) {
            ss << indentStr(indent + 3) << "\"module_type\": \"" << std::get<std::string>(node->data) << "\",\n";
        }

        // Add output_names for multi-output nodes
        if (!node->output_names.empty()) {
            ss << indentStr(indent + 3) << "\"output_names\": [";
            for (size_t j = 0; j < node->output_names.size(); ++j) {
                ss << "\"" << node->output_names[j] << "\"";
                if (j < node->output_names.size() - 1) ss << ", ";
            }
            ss << "],\n";
        }

        // Add type info if available
        if (node->hasType()) {
            ss << indentStr(indent + 3) << "\"type\": {\"width\": " << node->type->width
               << ", \"signed\": " << (node->type->isSigned() ? "true" : "false") << "},\n";
        }

        // Add source location if available
        if (node->loc) {
            ss << indentStr(indent + 3) << "\"loc\": \"" << node->loc->str() << "\",\n";
        }

        // Add inputs
        ss << indentStr(indent + 3) << "\"inputs\": [";
        for (size_t j = 0; j < node->in.size(); ++j) {
            if (j > 0) ss << ", ";
            size_t k = nodeIndex.at(node->in[j].node);
            if (node->in[j].port != 0) {
                ss << "{\"node\": " << k << ", \"port\": " << node->in[j].port << "}";
            } else {
                ss << k;
            }
        }
        ss << "]\n";

        ss << indentStr(indent + 2) << "}";
    }

    ss << "\n" << indentStr(indent + 1) << "]\n";
    ss << indentStr(indent) << "}";
    return ss.str();
}

std::string DFG::toJson(int indent) const {
    return renderJson(indent, nullptr);
}

std::string DFG::toJsonCone(const DFGNode* root, int indent) const {
    std::set<const DFGNode*> cone;
    std::queue<const DFGNode*> q;
    cone.insert(root);
    q.push(root);
    while (!q.empty()) {
        const DFGNode* curr = q.front();
        q.pop();
        for (const auto& input : curr->in) {
            if (cone.insert(input.node).second) {
                q.push(input.node);
            }
        }
    }
    return renderJson(indent, &cone);
}

} // namespace custom_hdl
