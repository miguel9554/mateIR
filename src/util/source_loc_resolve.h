#pragma once

#include "util/source_loc.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceManager.h"

namespace mate {

inline SourceLoc resolveSourceLoc(const slang::syntax::SyntaxNode& node,
                                  const slang::SourceManager& sm) {
    auto loc = node.sourceRange().start();
    return SourceLoc{
        .file = std::string(sm.getFileName(loc)),
        .line = sm.getLineNumber(loc),
        .column = sm.getColumnNumber(loc),
    };
}

} // namespace mate
