#include "frontends/systemverilog/systemverilog_frontend.h"

#include "frontends/systemverilog/systemverilog_pipeline.h"
#include "frontends/systemverilog/passes/extractor.h"
#include "util/source_loc.h"

#include "slang/syntax/SyntaxTree.h"

#include <memory>
#include <span>
#include <string_view>

namespace custom_hdl {

std::string SystemVerilogFrontend::name() const {
    return "systemverilog";
}

MateIR SystemVerilogFrontend::compile(const FrontendOptions& options) const {
    std::shared_ptr<slang::syntax::SyntaxTree> tree;

    if (options.source_files.empty()) {
        throw CompilerError("SystemVerilog frontend requires at least one source file");
    }

    if (options.source_files.size() == 1) {
        auto treeResult = slang::syntax::SyntaxTree::fromFile(options.source_files.front());
        if (!treeResult) {
            throw CompilerError("Failed to load file: " + options.source_files.front());
        }
        tree = std::move(treeResult).value();
    } else {
        std::vector<std::string_view> paths(options.source_files.begin(),
                                            options.source_files.end());
        auto treeResult = slang::syntax::SyntaxTree::fromFiles(std::span<const std::string_view>(paths));
        if (!treeResult) {
            throw CompilerError("Failed to load source files");
        }
        tree = std::move(treeResult).value();
    }

    auto& diagnostics = tree->diagnostics();
    bool hasErrors = false;
    for (const auto& diag : diagnostics) {
        if (diag.isError()) {
            hasErrors = true;
            break;
        }
    }
    if (hasErrors) {
        throw CompilerError("Syntax errors found: " + std::to_string(diagnostics.size()) + " diagnostic(s)");
    }

    auto extracted = buildIR(*tree);
    return lowerSystemVerilogToMateIR(extracted, tree->sourceManager(), options);
}

} // namespace custom_hdl
