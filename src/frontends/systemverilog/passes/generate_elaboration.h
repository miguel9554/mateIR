#pragma once

// Generate-block elaboration for pass 2: NBA target scanning, generate-scope
// declaration pre-population, and generate member resolution.
// Extracted verbatim from elaboration.cpp; internal to the elaboration pass.

#include "frontends/systemverilog/passes/elaboration_internal.h"

namespace mate {

void collectGenerateNBATargetsFromMember(const slang::syntax::MemberSyntax* member,
                                         const ParameterContext& ctx,
                                         const slang::SourceManager& sm,
                                         std::set<std::string>& out);

void resolveGenerateMembersInPlace(
        const slang::syntax::SyntaxList<slang::syntax::MemberSyntax>& members,
        ResolutionContext& ctx);

void resolveGenerateMemberInPlace(
        const slang::syntax::MemberSyntax* member,
        ResolutionContext& ctx);

} // namespace mate
