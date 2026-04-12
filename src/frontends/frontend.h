#pragma once

#include "passes/extractor.h"

#include <memory>
#include <string>
#include <vector>

namespace slang::syntax {
class SyntaxTree;
}

namespace custom_hdl {

struct FrontendOptions {
    std::vector<std::string> source_files;
};

struct FrontendOutput {
    ExtractedIR extracted_ir;

    // Current unresolved IR nodes still reference slang syntax objects, so the
    // SystemVerilog syntax tree must outlive mateIR compilation. A future
    // language-neutral unresolved IR can remove this field from the interface.
    std::shared_ptr<slang::syntax::SyntaxTree> systemverilog_syntax_tree;
};

class Frontend {
public:
    virtual ~Frontend() = default;
    virtual std::string name() const = 0;
    virtual FrontendOutput parse(const FrontendOptions& options) const = 0;
};

} // namespace custom_hdl
