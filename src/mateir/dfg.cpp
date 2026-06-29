#include "mateir/dfg.h"

#include <map>
#include <queue>
#include <set>
#include <sstream>

namespace mate {

namespace {

std::string typeToJson(const Type& t) {
    std::ostringstream ss;
    ss << "{";
    const char* kind = "integer";
    if (t.kind == TypeKind::Enum) kind = "enum";
    else if (t.kind == TypeKind::Struct) kind = "struct";
    ss << "\"kind\": \"" << kind << "\", ";
    ss << "\"width\": " << t.width << ", ";
    ss << "\"signed\": " << (t.isSigned() ? "true" : "false");
    if (!t.packed_dims.empty()) {
        ss << ", \"packed_dims\": [";
        for (size_t i = 0; i < t.packed_dims.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"left\": " << t.packed_dims[i].left
               << ", \"right\": " << t.packed_dims[i].right << "}";
        }
        ss << "]";
    }
    if (!t.unpacked_dims.empty()) {
        ss << ", \"unpacked_dims\": [";
        for (size_t i = 0; i < t.unpacked_dims.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"left\": " << t.unpacked_dims[i].left
               << ", \"right\": " << t.unpacked_dims[i].right << "}";
        }
        ss << "]";
    }
    if (t.kind == TypeKind::Enum) {
        const auto& ei = t.enumInfo();
        ss << ", \"enum_type\": \"" << ei.type_name << "\"";
        ss << ", \"enum_members\": [";
        for (size_t i = 0; i < ei.members.size(); ++i) {
            if (i) ss << ", ";
            ss << "{\"name\": \"" << ei.members[i].name
               << "\", \"value\": " << ei.members[i].value << "}";
        }
        ss << "]";
    } else if (t.kind == TypeKind::Struct) {
        const auto& si = t.structInfo();
        ss << ", \"struct_type\": \"" << si.type_name << "\"";
        ss << ", \"struct_identity\": \"" << si.type_identity << "\"";
        ss << ", \"struct_fields\": [";
        for (size_t i = 0; i < si.fields.size(); ++i) {
            if (i) ss << ", ";
            ss << "{";
            ss << "\"name\": \"" << si.fields[i].name << "\", ";
            if (!si.fields[i].type) {
                ss << "\"type\": null";
            } else {
                ss << "\"type\": " << typeToJson(*si.fields[i].type);
            }
            ss << "}";
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

} // namespace

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

        switch (node->kind()) {
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
                ss << node->constValue();
                break;
            case DFGOp::X:
                ss << "X";
                if (!node->name.empty()) ss << "\\n" << node->name;
                break;
            case DFGOp::ADD: ss << "+"; break;
            case DFGOp::SUB: ss << "-"; break;
            case DFGOp::MUL: ss << "*"; break;
            case DFGOp::EQ:  ss << "=="; break;
            case DFGOp::LT:  ss << "<"; break;
            case DFGOp::LE:  ss << "<="; break;
            case DFGOp::GT:  ss << ">"; break;
            case DFGOp::GE:  ss << ">="; break;
            case DFGOp::SHL: ss << "<<"; break;
            case DFGOp::SHR: ss << ">>"; break;
            case DFGOp::ASR: ss << ">>>"; break;
            case DFGOp::MUX: ss << "MUX"; break;
            case DFGOp::SLICE: ss << "SLICE"; break;
            case DFGOp::CONCAT: ss << "CONCAT"; break;
            case DFGOp::UNARY_NEGATE: ss << "-"; break;
            case DFGOp::BITWISE_NOT: ss << "~"; break;
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

    // Returns the label for the j-th input edge of a positional node.
    auto inputLabel = [](const DFGNode* node, size_t j) -> std::string {
        switch (node->kind()) {
            case DFGOp::MUX:
                if (j == 0) return "sel";
                if (j > 0 && j - 1 < node->muxValues().size()) {
                    return "d[" + std::to_string(node->muxValues()[j - 1]) + "]";
                }
                break;
            case DFGOp::SLICE:
                if (j == 0) return "src";
                if (j == 1) return "hi";
                if (j == 2) return "lo";
                break;
            default:
                break;
        }
        return "";
    };

    // Output edges
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (filter && !filter->count(node.get())) continue;

        for (size_t j = 0; j < node->rawInputs().size(); ++j) {
            const auto& input = node->rawInputs()[j];
            if (filter && !filter->count(input.node)) continue;
            ss << "  n" << nodeIndex.at(input.node) << " -> n" << i;
            std::string label = inputLabel(node.get(), j);
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
        for (const auto& input : curr->rawInputs()) {
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
        ss << indentStr(indent + 3) << "\"debug_id\": " << node->debug_id << ",\n";
        ss << indentStr(indent + 3) << "\"op\": \"" << to_string(node->kind()) << "\",\n";

        // Add name if present
        if (!node->name.empty()) {
            ss << indentStr(indent + 3) << "\"name\": \"" << node->name << "\",\n";
        }
        if (!node->instance_path.empty()) {
            ss << indentStr(indent + 3) << "\"instance_path\": \"" << node->instance_path << "\",\n";
        }
        const std::string full_name = node->debugName();
        if (!full_name.empty()) {
            ss << indentStr(indent + 3) << "\"full_name\": \"" << full_name << "\",\n";
        }
        // Add data field based on variant type
        if (node->kind() == DFGOp::CONST) {
            ss << indentStr(indent + 3) << "\"value\": " << node->constValue() << ",\n";
        }
        // Add type info if available
        if (node->hasType()) {
            ss << indentStr(indent + 3) << "\"type\": " << typeToJson(*node->type) << ",\n";
        }

        // Add source location if available
        if (node->loc) {
            ss << indentStr(indent + 3) << "\"loc\": \"" << node->loc->str() << "\",\n";
        }

        if (node->kind() == DFGOp::MUX && !node->muxValues().empty()) {
            ss << indentStr(indent + 3) << "\"mux_selector_values\": [";
            const auto& muxValues = node->muxValues();
            for (size_t j = 0; j < muxValues.size(); ++j) {
                if (j > 0) ss << ", ";
                ss << muxValues[j];
            }
            ss << "],\n";
        }

        // Add inputs
        ss << indentStr(indent + 3) << "\"inputs\": [";
        for (size_t j = 0; j < node->rawInputs().size(); ++j) {
            if (j > 0) ss << ", ";
            size_t k = nodeIndex.at(node->rawInputs()[j].node);
            ss << "{"
               << "\"role\": \"" << dfgInputRole(*node, j) << "\", "
               << "\"node\": " << k << ", "
               << "\"debug_id\": " << node->rawInputs()[j].node->debug_id;
            if (node->rawInputs()[j].port != 0) {
                ss << ", \"port\": " << node->rawInputs()[j].port;
            }
            ss << "}";
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
        for (const auto& input : curr->rawInputs()) {
            if (cone.insert(input.node).second) {
                q.push(input.node);
            }
        }
    }
    return renderJson(indent, &cone);
}

} // namespace mate
