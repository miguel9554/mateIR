#include "mateir/unresolved.h"
#include "util/debug.h"

#include <iostream>

#include "slang/syntax/AllSyntax.h"

namespace custom_hdl {

void UnresolvedType::print(std::ostream& os, bool debug) const {
    if (syntax) {
        if (debug) os << "\n";
        // Avoid printing Trivia (comments), just rawText of tokens.
        for (auto it = syntax->tokens_begin(); it != syntax->tokens_end(); ++it) {
            if (debug){
                os << "Token: ";
                os << (*it).rawText();
                os << "\n";
            } else {
                os << (*it).rawText();
                os << " ";
            }
        }
    } else {
        os << "<no type>";
    }
}

void UnresolvedSignal::print(std::ostream& os) const {
    os << name << ": ";
    os << "type (";
    type.print(os, 0);
    if (dimensions.syntax) os << ") dim (" << dimensions.syntax->toString() << ")";
}

void UnresolvedParam::print(std::ostream& os) const {
    os << name << ": ";
    os << "type (";
    type.print(os, 0);
    if (dimensions.syntax) os << ") dim (" << dimensions.syntax->toString() << ")";
    if (defaultValue) {
        os << " = " << defaultValue->toString();
    }
}

void UnresolvedModule::print(int indent) const {
    auto indent_str = [](int n) { return std::string(n * 2, ' '); };

    std::cout << indent_str(indent) << "Module: " << this->name << std::endl;

    std::cout << indent_str(indent + 1) << "Parameters:" << std::endl;
    for (const auto& param : this->parameters) {
        std::cout << indent_str(indent + 2);
        param.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Localparams:" << std::endl;
    for (const auto& param : this->localparams) {
        std::cout << indent_str(indent + 2);
        param.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Inputs:" << std::endl;
    for (const auto& in : this->inputs) {
        std::cout << indent_str(indent + 2);
        in.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Outputs:" << std::endl;
    for (const auto& out : this->outputs) {
        std::cout << indent_str(indent + 2);
        out.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Signals:" << std::endl;
    for (const auto& signal : this->signals) {
        std::cout << indent_str(indent + 2);
        signal.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "Flops:" << std::endl;
    for (const auto& flop : this->flops) {
        std::cout << indent_str(indent + 2);
        flop.print(std::cout);
        std::cout << std::endl;
    }

    std::cout << indent_str(indent + 1) << "No of procedural timing: " << this->proceduralTimingBlocks.size() << std::endl;
    std::cout << indent_str(indent + 1) << "No of procedural combo: " << this->proceduralComboBlocks.size() << std::endl;
    std::cout << indent_str(indent + 1) << "No of cont. assign.: " << this->assignStatements.size() << std::endl;
    std::cout << indent_str(indent + 1) << "No of hier. inst.: " << this->hierarchyInstantiation.size() << std::endl;

    // Ensure output directory exists
    ensureDebugOutputDir();

    // Serialize assign statements to files
    if (g_dump_unresolved_assign) {
        for (size_t i = 0; i < this->assignStatements.size(); ++i) {
            std::string filename = DEBUG_OUTPUT_DIR + "/" + this->name + "_unresolved_assign_" + std::to_string(i) + ".json";
            dumpSyntaxNodeToJson(filename, this->assignStatements[i]);
            std::cout << indent_str(indent + 1) << "Wrote assign " << i << " to: " << filename << std::endl;
        }
    }

    // Serialize hierarchical instantiations to files
    if (g_dump_unresolved_hierarchy) {
        for (size_t i = 0; i < this->hierarchyInstantiation.size(); ++i) {
            std::string filename = DEBUG_OUTPUT_DIR + "/" + this->name + "_unresolved_hierarchy_" + std::to_string(i) + ".json";
            dumpSyntaxNodeToJson(filename, this->hierarchyInstantiation[i]);
            std::cout << indent_str(indent + 1) << "Wrote hierarchy instantiation " << i << " to: " << filename << std::endl;
        }
    }
}

} // namespace custom_hdl
