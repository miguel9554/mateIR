#include "frontends/systemverilog/passes/constant_eval.h"

#include "frontends/systemverilog/syntax_helpers.h"
#include "util/source_loc_resolve.h"

#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxKind.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace slang::syntax;

namespace mate {

ConstantValue integerConstant(int64_t value) {
    return ConstantValue::bits(Type::makeInteger(64, true), value);
}

namespace {

static int64_t intPowConst(int64_t base, int64_t exp) {
    if (exp < 0)
        throw std::runtime_error("negative exponent");
    if (base == 0 && exp == 0)
        throw std::runtime_error("0**0");
    int64_t result = 1;
    for (int64_t i = 0; i < exp; i++) {
        result *= base;
    }
    return result;
}

size_t constantWordCount(int width) {
    if (width <= 0) {
        throw CompilerError("Constant bit vector must have a positive width");
    }
    return (static_cast<size_t>(width) + 63) / 64;
}

void maskConstantHighBits(std::vector<uint64_t>& words, int width) {
    const int highBits = width % 64;
    if (highBits != 0) words.back() &= (uint64_t(1) << highBits) - 1;
}

uint8_t hexDigitValue(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<uint8_t>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<uint8_t>(10 + ch - 'a');
    if (ch >= 'A' && ch <= 'F') return static_cast<uint8_t>(10 + ch - 'A');
    throw CompilerError("Unsupported hex digit in string literal escape");
}

void appendDecodedEscape(std::string_view text, size_t& index, std::vector<uint8_t>& bytes) {
    if (index >= text.size()) {
        throw CompilerError("Unterminated escape sequence in string literal");
    }

    const char ch = text[index++];
    switch (ch) {
        case 'n': bytes.push_back('\n'); return;
        case 't': bytes.push_back('\t'); return;
        case 'r': bytes.push_back('\r'); return;
        case 'f': bytes.push_back('\f'); return;
        case 'v': bytes.push_back('\v'); return;
        case 'a': bytes.push_back('\a'); return;
        case 'b': bytes.push_back('\b'); return;
        case '\\': bytes.push_back('\\'); return;
        case '"': bytes.push_back('"'); return;
        case '\'': bytes.push_back('\''); return;
        case 'x': {
            if (index >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[index]))) {
                throw CompilerError("Hex string literal escape requires at least one digit");
            }
            uint8_t value = 0;
            int digits = 0;
            while (index < text.size() &&
                   std::isxdigit(static_cast<unsigned char>(text[index])) &&
                   digits < 2) {
                value = static_cast<uint8_t>((value << 4) | hexDigitValue(text[index]));
                ++index;
                ++digits;
            }
            bytes.push_back(value);
            return;
        }
        default:
            break;
    }

    if (ch >= '0' && ch <= '7') {
        uint8_t value = static_cast<uint8_t>(ch - '0');
        int digits = 1;
        while (index < text.size() &&
               text[index] >= '0' && text[index] <= '7' &&
               digits < 3) {
            value = static_cast<uint8_t>((value << 3) | static_cast<uint8_t>(text[index] - '0'));
            ++index;
            ++digits;
        }
        bytes.push_back(value);
        return;
    }

    throw CompilerError("Unsupported escape sequence in string literal");
}

std::vector<uint8_t> decodeFrontendStringLiteral(std::string_view text) {
    if (text.size() >= 6 && text.starts_with("\"\"\"") && text.ends_with("\"\"\"")) {
        std::vector<uint8_t> bytes;
        bytes.reserve(text.size() - 6);
        for (size_t i = 3; i + 3 < text.size(); ++i) {
            bytes.push_back(static_cast<uint8_t>(text[i]));
        }
        return bytes;
    }

    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        throw CompilerError("Malformed string literal");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() - 2);
    for (size_t i = 1; i + 1 < text.size();) {
        if (text[i] == '\\') {
            ++i;
            appendDecodedEscape(text, i, bytes);
            continue;
        }
        bytes.push_back(static_cast<uint8_t>(text[i]));
        ++i;
    }
    return bytes;
}

std::vector<uint64_t> lowerFrontendStringLiteralToWords(std::string_view text, int width) {
    std::vector<uint64_t> words(constantWordCount(width), 0);
    const auto bytes = decodeFrontendStringLiteral(text);
    const int maxBytes = width / 8;
    const int copyBytes = std::min<int>(static_cast<int>(bytes.size()), maxBytes);
    const int start = static_cast<int>(bytes.size()) - copyBytes;
    for (int i = 0; i < copyBytes; ++i) {
        const uint8_t value = bytes[static_cast<size_t>(start + i)];
        const int byteIndex = copyBytes - 1 - i;
        const int bitOffset = byteIndex * 8;
        words[bitOffset / 64] |= uint64_t(value) << (bitOffset % 64);
        if ((bitOffset % 64) > 56) {
            words[bitOffset / 64 + 1] |= uint64_t(value) >> (64 - (bitOffset % 64));
        }
    }
    maskConstantHighBits(words, width);
    return words;
}

ConstantValue lowerStringLiteralConstant(std::string_view text, const Type& expectedType) {
    if (expectedType.isStruct() || !expectedType.unpacked_dims.empty()) {
        throw CompilerError("String literal constant requires an integral destination type");
    }
    if (expectedType.packed_dims.size() > 1) {
        throw CompilerError(
            "String literal constants are not supported for multidimensional packed arrays");
    }
    if (expectedType.width <= 0) {
        throw CompilerError("Integral constant type must have a positive width");
    }
    return ConstantValue::bitWords(
        expectedType, lowerFrontendStringLiteralToWords(text, expectedType.width));
}

}  // namespace

IntegerVectorLiteral parseIntegerVectorExpression(const IntegerVectorExpressionSyntax& vecExpr){
    std::string sizeText(vecExpr.size.rawText());
    std::string baseText(vecExpr.base.rawText());
    std::string valueText(vecExpr.value.rawText());
    std::string literal = sizeText + baseText + valueText;

    // Remove underscores from value (Verilog allows 8'hFF_FF)
    valueText.erase(std::remove(valueText.begin(), valueText.end(), '_'), valueText.end());

    int base = 10;
    if (baseText.find('h') != std::string::npos || baseText.find('H') != std::string::npos) {
        base = 16;
    } else if (baseText.find('b') != std::string::npos || baseText.find('B') != std::string::npos) {
        base = 2;
    } else if (baseText.find('o') != std::string::npos || baseText.find('O') != std::string::npos) {
        base = 8;
    } else if (baseText.find('d') != std::string::npos || baseText.find('D') != std::string::npos) {
        base = 10;
    }

    int64_t value = std::stoll(valueText, nullptr, base);

    int width;
    if (!sizeText.empty()) {
        width = std::stoi(sizeText);
    } else {
        // Unsized literal: compute the minimum bits needed to represent the value.
        bool is_signed_for_width = baseText.find('s') != std::string::npos ||
                                   baseText.find('S') != std::string::npos;
        if (is_signed_for_width) {
            if (value == 0 || value == -1) {
                width = 1;
            } else if (value > 0) {
                // Positive signed: need sign bit → floor(log2(value)) + 2
                width = (64 - __builtin_clzll(static_cast<uint64_t>(value))) + 1;
            } else {
                // Negative (< -1): floor(log2(|value| - 1)) + 2
                uint64_t abs_minus_1 = static_cast<uint64_t>(-(value + 1));
                width = (64 - __builtin_clzll(abs_minus_1)) + 1;
            }
        } else {
            if (value == 0) {
                width = 1;
            } else {
                // Unsigned: floor(log2(value)) + 1
                width = 64 - __builtin_clzll(static_cast<uint64_t>(value));
            }
        }
    }
    bool is_signed = baseText.find('s') != std::string::npos ||
                     baseText.find('S') != std::string::npos;
    return {value, width, is_signed};
}

CasezItemPattern parseCasezItemPattern(
    const std::string& valueText, int base,
    const std::optional<int>& explicitWidth,
    const std::optional<SourceLoc>& loc)
{
    int64_t value = 0;
    int64_t wildcard_mask = 0;

    if (base == 2) {
        int bit = 0;
        for (int i = static_cast<int>(valueText.size()) - 1; i >= 0; --i) {
            char c = valueText[i];
            if (bit >= 64)
                throw CompilerError("casez binary pattern exceeds 63 bits", loc);
            if (c == '1') {
                value |= (1LL << bit);
            } else if (c == '0') {
                // nothing
            } else if (c == '?' || c == 'z' || c == 'Z') {
                wildcard_mask |= (1LL << bit);
            } else if (c == 'x' || c == 'X') {
                throw CompilerError(
                    "'x' bits are not supported in casez items; use '?' for don't-care", loc);
            } else {
                throw CompilerError(
                    std::format("Invalid character '{}' in casez binary literal", c), loc);
            }
            ++bit;
        }
    } else if (base == 16) {
        int bit = 0;
        for (int i = static_cast<int>(valueText.size()) - 1; i >= 0; --i) {
            char c = valueText[i];
            if (bit >= 64)
                throw CompilerError("casez hex pattern exceeds 63 bits", loc);
            if (c == '?' || c == 'z' || c == 'Z') {
                wildcard_mask |= (0xFLL << bit);
            } else if (c == 'x' || c == 'X') {
                throw CompilerError(
                    "'x' bits are not supported in casez items; use '?' for don't-care", loc);
            } else {
                int digit = 0;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else throw CompilerError(
                    std::format("Invalid character '{}' in casez hex literal", c), loc);
                value |= (static_cast<int64_t>(digit) << bit);
            }
            bit += 4;
        }
    } else {
        throw CompilerError(
            "casez wildcard patterns are only supported in binary or hex literals", loc);
    }

    int width = explicitWidth.value_or(0);
    if (width > 0 && width < 64) {
        int64_t mask = (1LL << width) - 1;
        value &= mask;
        wildcard_mask &= mask;
    }
    return {value, wildcard_mask, width};
}

int constantExprWidth(const ExpressionSyntax* expr,
                             const slang::SourceManager* sm,
                             const ParameterContext* paramCtx,
                             const NamedTypeRegistry* namedTypeRegistry,
                             const PackageRegistry* pkgRegistry) {
    if (!expr) throw CompilerError("Cannot determine width of null constant expression");
    auto loc = sm ? std::optional<SourceLoc>(resolveSourceLoc(*expr, *sm)) : std::nullopt;
    switch (expr->kind) {
        case SyntaxKind::IntegerLiteralExpression:
            return 32;
        case SyntaxKind::IntegerVectorExpression: {
            const auto& literal = expr->as<IntegerVectorExpressionSyntax>();
            if (!literal.size.rawText().empty()) {
                return std::stoi(std::string(literal.size.rawText()));
            }
            return parseIntegerVectorExpression(literal).width;
        }
        case SyntaxKind::StringLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            const auto bytes = decodeFrontendStringLiteral(literal.literal.rawText());
            if (bytes.empty()) return 0;
            if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 8)) {
                throw CompilerError("String literal width exceeds supported range", loc);
            }
            return static_cast<int>(bytes.size() * 8);
        }
        case SyntaxKind::ParenthesizedExpression:
            return constantExprWidth(expr->as<ParenthesizedExpressionSyntax>().expression,
                                     sm, paramCtx, namedTypeRegistry, pkgRegistry);
        case SyntaxKind::ConcatenationExpression: {
            int width = 0;
            for (const auto* item : expr->as<ConcatenationExpressionSyntax>().expressions) {
                const int itemWidth = constantExprWidth(item, sm, paramCtx,
                                                        namedTypeRegistry, pkgRegistry);
                if (itemWidth > std::numeric_limits<int>::max() - width) {
                    throw CompilerError("Constant concatenation width exceeds supported range");
                }
                width += itemWidth;
            }
            return width;
        }
        case SyntaxKind::IdentifierName: {
            std::string name(expr->as<IdentifierNameSyntax>().identifier.valueText());
            if (namedTypeRegistry) {
                auto it = namedTypeRegistry->find(name);
                if (it != namedTypeRegistry->end()) return bitstreamWidth(it->second);
            }
            if (paramCtx) {
                auto it = paramCtx->values.find(name);
                if (it != paramCtx->values.end()) return bitstreamWidth(it->second.type());
            }
            throw CompilerError(
                "Cannot determine width of constant expression: " + name, loc);
        }
        case SyntaxKind::ScopedName: {
            if (!pkgRegistry) break;
            const auto& scoped = expr->as<ScopedNameSyntax>();
            if (scoped.separator.rawText() != "::") break;
            std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string itemName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = pkgRegistry->find(pkgName);
            if (pkgIt == pkgRegistry->end()) break;
            auto typeIt = pkgIt->second.namedTypes.find(itemName);
            if (typeIt != pkgIt->second.namedTypes.end()) return bitstreamWidth(typeIt->second);
            auto constIt = pkgIt->second.constants.find(itemName);
            if (constIt != pkgIt->second.constants.end()) return bitstreamWidth(constIt->second.type());
            break;
        }
        default:
            break;
    }
    throw CompilerError(
        "Cannot determine width of constant expression: " +
        std::string(toString(expr->kind)), loc);
}

int64_t bitstreamWidth(const Type& type) {
    int64_t width = 0;
    if (type.isStruct()) {
        for (const auto& field : type.structInfo().fields) {
            width += bitstreamWidth(*field.type);
        }
    } else {
        width = type.width;
    }
    if (width <= 0) {
        throw CompilerError("Cannot determine bit-stream width for type");
    }
    for (const auto& dim : type.unpacked_dims) {
        width *= static_cast<int64_t>(dim.size());
    }
    return width;
}

const ExpressionSyntax* singleOrderedSystemFunctionArg(
    const InvocationExpressionSyntax& invocation,
    std::string_view functionName) {
    if (!invocation.arguments || invocation.arguments->parameters.size() != 1 ||
        invocation.arguments->parameters[0]->kind != SyntaxKind::OrderedArgument) {
        throw CompilerError(std::string(functionName) + " requires exactly one ordered argument");
    }
    return extractPortExpr(
        *invocation.arguments->parameters[0]->as<OrderedArgumentSyntax>().expr);
}

std::optional<int64_t> staticBitsWidth(
    const ExpressionSyntax* expr,
    const ParameterContext& ctx,
    const PackageRegistry* pkgRegistry,
    const NamedTypeRegistry* namedTypeRegistry) {
    if (!expr) throw CompilerError("$bits argument cannot be null");
    if (expr->kind == SyntaxKind::IdentifierName) {
        std::string name(expr->as<IdentifierNameSyntax>().identifier.valueText());
        if (namedTypeRegistry) {
            auto typeIt = namedTypeRegistry->find(name);
            if (typeIt != namedTypeRegistry->end()) return bitstreamWidth(typeIt->second);
        }
        auto valueIt = ctx.values.find(name);
        if (valueIt != ctx.values.end()) return bitstreamWidth(valueIt->second.type());
        return std::nullopt;
    }
    if (expr->kind == SyntaxKind::ScopedName) {
        const auto& scoped = expr->as<ScopedNameSyntax>();
        if (scoped.separator.rawText() != "::") return std::nullopt;
        if (!pkgRegistry) return std::nullopt;
        std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
        std::string itemName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
        auto pkgIt = pkgRegistry->find(pkgName);
        if (pkgIt == pkgRegistry->end()) return std::nullopt;
        auto typeIt = pkgIt->second.namedTypes.find(itemName);
        if (typeIt != pkgIt->second.namedTypes.end()) return bitstreamWidth(typeIt->second);
        auto constantIt = pkgIt->second.constants.find(itemName);
        if (constantIt != pkgIt->second.constants.end()) return bitstreamWidth(constantIt->second.type());
        auto enumIt = pkgIt->second.enumMembers.find(itemName);
        if (enumIt != pkgIt->second.enumMembers.end()) return bitstreamWidth(enumIt->second.second);
    }
    return std::nullopt;
}

// Evaluate a constant expression given a parameter context
// Throws if a referenced parameter is not in the context
int64_t evaluateConstantExpr(const ExpressionSyntax* expr, const ParameterContext& ctx,
                              const PackageRegistry* pkgRegistry,
                              const NamedTypeRegistry* namedTypeRegistry,
                              const slang::SourceManager* sm,
                              std::source_location caller) {
    if (!expr) {
        throw CompilerError(
            std::string("Cannot evaluate null expression (called from ") +
            caller.file_name() + ":" + std::to_string(caller.line()) + ")");
    }

    switch (expr->kind) {
        case SyntaxKind::IntegerLiteralExpression: {
            auto& literal = expr->as<LiteralExpressionSyntax>();
            // Parse the integer literal token
            auto text = literal.literal.rawText();
            // Handle simple decimal integers for now
            // TODO: handle other bases (hex, octal, binary) and sized literals
            return std::stoll(std::string(text));
        }
        case SyntaxKind::StringLiteralExpression: {
            auto& literal = expr->as<LiteralExpressionSyntax>();
            const auto text = literal.literal.rawText();
            const auto bytes = decodeFrontendStringLiteral(text);
            if (bytes.empty()) return 0;
            if (bytes.size() > 8) {
                throw CompilerError(
                    "String literal constant does not fit in int64_t",
                    sm ? std::optional<SourceLoc>(resolveSourceLoc(*expr, *sm)) : std::nullopt);
            }
            Type stringType = Type::makeInteger(static_cast<int>(bytes.size() * 8), false);
            return lowerStringLiteralConstant(text, stringType).requireInt64(
                "String literal constant",
                sm ? std::optional<SourceLoc>(resolveSourceLoc(*expr, *sm)) : std::nullopt);
        }

        case SyntaxKind::IdentifierName: {
            auto& name = expr->as<IdentifierNameSyntax>();
            std::string paramName(name.identifier.valueText());
            if (namedTypeRegistry && namedTypeRegistry->contains(paramName)) {
                throw CompilerError("Type name '" + paramName + "' cannot be used as an integer constant");
            }
            auto it = ctx.values.find(paramName);
            if (it == ctx.values.end()) {
                throw CompilerError(
                    "Parameter '" + paramName + "' not found in context");
            }
            return it->second.requireInt64("Parameter '" + paramName + "'");
        }

        case SyntaxKind::MemberAccessExpression: {
            // Qualified constant access, e.g. an interface port parameter
            // `_my_modport_if.W` stored in the context as "_my_modport_if.W".
            auto& member = expr->as<MemberAccessExpressionSyntax>();
            if (member.left->kind == SyntaxKind::IdentifierName) {
                std::string qualified =
                    std::string(member.left->as<IdentifierNameSyntax>().identifier.valueText()) +
                    "." + std::string(member.name.valueText());
                auto it = ctx.values.find(qualified);
                if (it != ctx.values.end()) {
                    return it->second.requireInt64("Parameter '" + qualified + "'");
                }
                throw CompilerError("Parameter '" + qualified + "' not found in context");
            }
            throw CompilerError(
                "Unsupported member access in constant expression: " +
                std::string(toString(member.left->kind)));
        }

        case SyntaxKind::ParenthesizedExpression: {
            auto& paren = expr->as<ParenthesizedExpressionSyntax>();
            return evaluateConstantExpr(paren.expression, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::UnaryPlusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::UnaryMinusExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return -evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::UnaryLogicalNotExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry, sm) == 0 ? 1 : 0;
        }

        case SyntaxKind::AddExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) +
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::SubtractExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) -
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::MultiplyExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) *
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::DivideExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto divisor = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
            if (divisor == 0) {
                throw CompilerError("Division by zero in constant expression");
            }
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) / divisor;
        }

        case SyntaxKind::ModExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            auto divisor = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
            if (divisor == 0) {
                throw CompilerError("Modulo by zero in constant expression");
            }
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) % divisor;
        }

        case SyntaxKind::IntegerVectorExpression: {
            auto& vecExpr = expr->as<IntegerVectorExpressionSyntax>();
            return parseIntegerVectorExpression(vecExpr).value;
        }

        case SyntaxKind::ConcatenationExpression: {
            uint64_t result = 0;
            int resultWidth = 0;
            for (const auto* item : expr->as<ConcatenationExpressionSyntax>().expressions) {
                const int itemWidth = constantExprWidth(item, sm, &ctx, namedTypeRegistry, pkgRegistry);
                if (itemWidth > 64 || resultWidth > 64 - itemWidth) {
                    throw CompilerError("Constant concatenation does not fit in int64_t");
                }
                const uint64_t itemValue = static_cast<uint64_t>(
                    evaluateConstantExpr(item, ctx, pkgRegistry, namedTypeRegistry, sm));
                const uint64_t mask = itemWidth == 64
                    ? UINT64_MAX
                    : (uint64_t(1) << itemWidth) - 1;
                result = itemWidth == 64
                    ? itemValue
                    : (result << itemWidth) | (itemValue & mask);
                resultWidth += itemWidth;
            }
            return static_cast<int64_t>(result);
        }

        case SyntaxKind::ScopedName: {
            const auto& scoped = expr->as<ScopedNameSyntax>();
            if (!isPackageScopedName(scoped)) {
                if (scoped.left->kind == SyntaxKind::IdentifierName &&
                    scoped.right->kind == SyntaxKind::IdentifierName) {
                    std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                    std::string fieldName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                    auto valueIt = ctx.values.find(baseName);
                    if (valueIt != ctx.values.end()) {
                        return valueIt->second.field(fieldName).requireInt64(baseName + "." + fieldName);
                    }
                    // Qualified constant, e.g. an interface port parameter
                    // stored in the context as "<port>.<param>".
                    auto qualifiedIt = ctx.values.find(baseName + "." + fieldName);
                    if (qualifiedIt != ctx.values.end()) {
                        return qualifiedIt->second.requireInt64(baseName + "." + fieldName);
                    }
                }
                throw CompilerError("Unsupported constant field selection");
            }
            if (!pkgRegistry) {
                throw CompilerError("Package-qualified constant requires package registry");
            }
            std::string pkgName  = std::string(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string itemName = std::string(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = pkgRegistry->find(pkgName);
            if (pkgIt == pkgRegistry->end()) {
                throw CompilerError("Unknown package: " + pkgName);
            }
            auto memberIt = pkgIt->second.enumMembers.find(itemName);
            if (memberIt != pkgIt->second.enumMembers.end()) return memberIt->second.first;
            auto constantIt = pkgIt->second.constants.find(itemName);
            if (constantIt != pkgIt->second.constants.end())
                return constantIt->second.requireInt64("Package constant " + pkgName + "::" + itemName);
            throw CompilerError("Unknown package member: " + pkgName + "::" + itemName);
        }

        case SyntaxKind::EqualityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) == evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm) ? 1 : 0;
        }
        case SyntaxKind::InequalityExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) != evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm) ? 1 : 0;
        }
        case SyntaxKind::LessThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) < evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm) ? 1 : 0;
        }
        case SyntaxKind::LessThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) <= evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm) ? 1 : 0;
        }
        case SyntaxKind::GreaterThanExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) > evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm) ? 1 : 0;
        }
        case SyntaxKind::GreaterThanEqualExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) >= evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm) ? 1 : 0;
        }
        case SyntaxKind::LogicalAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return (evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) && evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm)) ? 1 : 0;
        }
        case SyntaxKind::LogicalOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return (evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) || evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm)) ? 1 : 0;
        }
        case SyntaxKind::ConditionalExpression: {
            const auto& conditional = expr->as<ConditionalExpressionSyntax>();
            if (conditional.predicate->conditions.size() != 1) {
                throw CompilerError("Only single condition supported in constant ternary expression");
            }
            if (conditional.predicate->conditions[0]->matchesClause) {
                throw CompilerError("Matches clause not supported in constant ternary expression");
            }
            return evaluateConstantExpr(
                evaluateConstantExpr(conditional.predicate->conditions[0]->expr, ctx, pkgRegistry, namedTypeRegistry, sm)
                    ? conditional.left
                    : conditional.right,
                ctx, pkgRegistry, namedTypeRegistry);
        }

        case SyntaxKind::PowerExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return intPowConst(evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm),
                               evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm));
        }

        case SyntaxKind::BinaryOrExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) |
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
        }
        case SyntaxKind::BinaryAndExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) &
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
        }
        case SyntaxKind::BinaryXorExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) ^
                   evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
        }
        case SyntaxKind::BinaryXnorExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            return ~(evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) ^
                     evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm));
        }
        case SyntaxKind::UnaryBitwiseNotExpression: {
            auto& unary = expr->as<PrefixUnaryExpressionSyntax>();
            return ~evaluateConstantExpr(unary.operand, ctx, pkgRegistry, namedTypeRegistry, sm);
        }
        case SyntaxKind::UnbasedUnsizedLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            const auto text = literal.literal.rawText();
            const char ch = text.size() > 1 ? text[1] : '0';
            if (ch == '0') return 0;
            if (ch == '1') return -1;
            throw CompilerError(
                "Unsupported unbased unsized literal in integer constant evaluation: " +
                std::string(text),
                sm ? std::optional<SourceLoc>(resolveSourceLoc(*expr, *sm)) : std::nullopt);
        }
        case SyntaxKind::LogicalShiftLeftExpression:
        case SyntaxKind::ArithmeticShiftLeftExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            int64_t shift = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
            if (shift < 0 || shift >= 64) return 0;
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) << shift;
        }
        case SyntaxKind::LogicalShiftRightExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            int64_t shift = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
            if (shift < 0 || shift >= 64) return 0;
            return static_cast<int64_t>(
                static_cast<uint64_t>(evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm)) >> shift);
        }
        case SyntaxKind::ArithmeticShiftRightExpression: {
            auto& binary = expr->as<BinaryExpressionSyntax>();
            int64_t shift = evaluateConstantExpr(binary.right, ctx, pkgRegistry, namedTypeRegistry, sm);
            if (shift < 0 || shift >= 64) return 0;
            return evaluateConstantExpr(binary.left, ctx, pkgRegistry, namedTypeRegistry, sm) >> shift;
        }
        case SyntaxKind::CastExpression: {
            auto& castExpr = expr->as<CastExpressionSyntax>();
            int64_t val = evaluateConstantExpr(castExpr.right->expression, ctx, pkgRegistry, namedTypeRegistry, sm);
            // Width cast: mask to the specified number of bits
            if (castExpr.left->kind == SyntaxKind::IntegerLiteralExpression) {
                int64_t width = std::stoll(std::string(
                    castExpr.left->as<LiteralExpressionSyntax>().literal.rawText()));
                if (width > 0 && width < 64) {
                    val = static_cast<int64_t>(static_cast<uint64_t>(val) & ((uint64_t(1) << width) - 1));
                }
            }
            return val;
        }
        case SyntaxKind::SignedCastExpression: {
            auto& castExpr = expr->as<SignedCastExpressionSyntax>();
            return evaluateConstantExpr(castExpr.inner->expression, ctx, pkgRegistry, namedTypeRegistry, sm);
        }

        case SyntaxKind::InvocationExpression: {
            const auto& invocation = expr->as<InvocationExpressionSyntax>();
            if (invocation.left->kind != SyntaxKind::SystemName) {
                throw CompilerError("Only $clog2 is supported in constant system-function calls");
            }
            const std::string systemName(invocation.left->as<SystemNameSyntax>().systemIdentifier.valueText());
            if (systemName == "$bits") {
                const auto* argument = singleOrderedSystemFunctionArg(invocation, "$bits");
                if (auto width = staticBitsWidth(argument, ctx, pkgRegistry, namedTypeRegistry)) {
                    return *width;
                }
                throw CompilerError("$bits argument is not a known type or constant expression",
                                    sm ? std::optional<SourceLoc>(resolveSourceLoc(invocation, *sm))
                                       : std::nullopt);
            }
            if (systemName != "$clog2") {
                throw CompilerError("Only $clog2 and $bits are supported in constant system-function calls");
            }
            const auto* argument = singleOrderedSystemFunctionArg(invocation, "$clog2");
            int64_t value = evaluateConstantExpr(argument, ctx, pkgRegistry, namedTypeRegistry, sm);
            if (value < 0) {
                throw CompilerError("$clog2 argument must not be negative");
            }
            int64_t result = 0;
            for (int64_t remaining = value > 0 ? value - 1 : 0;
                 remaining > 0; remaining >>= 1) {
                ++result;
            }
            return result;
        }

        default:
            throw CompilerError(
                "Unsupported expression kind in constant evaluation: " +
                std::string(toString(expr->kind)),
                sm ? std::optional<SourceLoc>(resolveSourceLoc(*expr, *sm)) : std::nullopt);
    }
}

// Overload with source location: reports where the null/bad expression came from.
int64_t evaluateConstantExpr(const ExpressionSyntax* expr, const ParameterContext& ctx,
                              const slang::SourceManager& sm,
                              const slang::syntax::SyntaxNode& contextNode,
                              const PackageRegistry* pkgRegistry,
                              const NamedTypeRegistry* namedTypeRegistry) {
    if (!expr) {
        throw CompilerError("Cannot evaluate null expression",
                            resolveSourceLoc(contextNode, sm));
    }
    try {
        return evaluateConstantExpr(expr, ctx, pkgRegistry, namedTypeRegistry, &sm);
    } catch (const CompilerError& error) {
        if (error.loc) throw;
        throw CompilerError(error.what(), resolveSourceLoc(*expr, sm));
    }
}

ConstantValue evaluateConstantValue(const ExpressionSyntax* expr,
                                           const Type& expectedType,
                                           const ParameterContext& ctx,
                                           const PackageRegistry& pkgRegistry,
                                           const NamedTypeRegistry* namedTypeRegistry,
                                           const slang::SourceManager& sm) {
    if (!expr) throw CompilerError("Cannot evaluate null constant expression");
    switch (expr->kind) {
        case SyntaxKind::IdentifierName: {
            std::string name(expr->as<IdentifierNameSyntax>().identifier.valueText());
            auto it = ctx.values.find(name);
            if (it == ctx.values.end()) {
                throw CompilerError(
                    "Parameter '" + name + "' not found in context",
                    resolveSourceLoc(*expr, sm));
            }
            return it->second;
        }
        case SyntaxKind::ScopedName: {
            const auto& scoped = expr->as<ScopedNameSyntax>();
            if (!isPackageScopedName(scoped)) {
                if (scoped.left->kind == SyntaxKind::IdentifierName &&
                    scoped.right->kind == SyntaxKind::IdentifierName) {
                    std::string baseName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
                    std::string fieldName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
                    auto valueIt = ctx.values.find(baseName);
                    if (valueIt != ctx.values.end()) {
                        return valueIt->second.field(fieldName);
                    }
                    // Qualified constant, e.g. an interface port parameter
                    // stored in the context as "<port>.<param>".
                    auto qualifiedIt = ctx.values.find(baseName + "." + fieldName);
                    if (qualifiedIt != ctx.values.end()) {
                        return qualifiedIt->second;
                    }
                }
                throw CompilerError(
                    "Unsupported aggregate constant field selection",
                    resolveSourceLoc(*expr, sm));
            }
            std::string pkgName(scoped.left->as<IdentifierNameSyntax>().identifier.valueText());
            std::string itemName(scoped.right->as<IdentifierNameSyntax>().identifier.valueText());
            auto pkgIt = pkgRegistry.find(pkgName);
            if (pkgIt == pkgRegistry.end()) {
                throw CompilerError("Unknown package: " + pkgName, resolveSourceLoc(*expr, sm));
            }
            auto constantIt = pkgIt->second.constants.find(itemName);
            if (constantIt != pkgIt->second.constants.end()) return constantIt->second;
            auto enumIt = pkgIt->second.enumMembers.find(itemName);
            if (enumIt != pkgIt->second.enumMembers.end()) {
                return ConstantValue::bits(enumIt->second.second, enumIt->second.first);
            }
            throw CompilerError(
                "Unknown package member: " + pkgName + "::" + itemName,
                resolveSourceLoc(*expr, sm));
        }
        case SyntaxKind::UnbasedUnsizedLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            const auto text = literal.literal.rawText();
            return ConstantValue::fill(expectedType, text.size() > 1 && text[1] == '1');
        }
        case SyntaxKind::IntegerLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            return ConstantValue::integerLiteral(expectedType, literal.literal.rawText());
        }
        case SyntaxKind::StringLiteralExpression: {
            const auto& literal = expr->as<LiteralExpressionSyntax>();
            return lowerStringLiteralConstant(literal.literal.rawText(), expectedType);
        }
        case SyntaxKind::IntegerVectorExpression: {
            const auto& literal = expr->as<IntegerVectorExpressionSyntax>();
            return ConstantValue::vectorLiteral(expectedType, literal.size.rawText(),
                                                literal.base.rawText(), literal.value.rawText());
        }
        case SyntaxKind::ConcatenationExpression: {
            std::vector<ConstantValue> values;
            for (const auto* item : expr->as<ConcatenationExpressionSyntax>().expressions) {
                values.push_back(evaluateConstantValue(
                    item, Type::makeInteger(constantExprWidth(item, &sm, &ctx, namedTypeRegistry, &pkgRegistry), false),
                    ctx, pkgRegistry, namedTypeRegistry, sm));
            }
            return ConstantValue::concatenate(expectedType, values);
        }
        case SyntaxKind::AssignmentPatternExpression: {
            const auto& assignment = expr->as<AssignmentPatternExpressionSyntax>();
            if (!expectedType.unpacked_dims.empty()) {
                if (assignment.pattern->kind != SyntaxKind::SimpleAssignmentPattern) {
                    throw CompilerError(
                        "Only ordered array assignment patterns are supported for package constants",
                        resolveSourceLoc(*expr, sm));
                }
                const auto& pattern = assignment.pattern->as<SimpleAssignmentPatternSyntax>();
                Type elementType = expectedType;
                elementType.unpacked_dims.erase(elementType.unpacked_dims.begin());
                std::vector<ConstantValue> values;
                values.reserve(pattern.items.size());
                for (const auto* item : pattern.items) {
                    values.push_back(evaluateConstantValue(
                        item, elementType, ctx, pkgRegistry, namedTypeRegistry, sm));
                }
                return ConstantValue::array(expectedType, std::move(values));
            }
            if (!expectedType.isStruct()) {
                throw CompilerError(
                    "Assignment pattern requires an aggregate package constant type",
                    resolveSourceLoc(*expr, sm));
            }
            if (assignment.pattern->kind == SyntaxKind::SimpleAssignmentPattern) {
                const auto& pattern = assignment.pattern->as<SimpleAssignmentPatternSyntax>();
                const auto& fields = expectedType.structInfo().fields;
                if (pattern.items.size() != fields.size()) {
                    throw CompilerError(
                        "Struct assignment pattern field count does not match its declared type",
                        resolveSourceLoc(*expr, sm));
                }
                std::vector<ConstantValue> values;
                values.reserve(fields.size());
                for (size_t i = 0; i < fields.size(); ++i) {
                    values.push_back(evaluateConstantValue(
                        pattern.items[i], *fields[i].type, ctx, pkgRegistry, namedTypeRegistry, sm));
                }
                return ConstantValue::orderedStruct(expectedType, std::move(values));
            }
            if (assignment.pattern->kind == SyntaxKind::StructuredAssignmentPattern) {
                const auto& pattern = assignment.pattern->as<StructuredAssignmentPatternSyntax>();
                std::map<std::string, const ExpressionSyntax*> expressions;
                const ExpressionSyntax* defaultExpr = nullptr;
                for (const auto* item : pattern.items) {
                    if (item->key->kind == SyntaxKind::DefaultPatternKeyExpression ||
                        (item->key->kind == SyntaxKind::IdentifierName &&
                         item->key->as<IdentifierNameSyntax>().identifier.valueText() == "default")) {
                        if (defaultExpr) {
                            throw CompilerError(
                                "Struct assignment pattern has multiple default keys",
                                resolveSourceLoc(*item, sm));
                        }
                        defaultExpr = item->expr;
                        continue;
                    }
                    if (item->key->kind != SyntaxKind::IdentifierName) {
                        throw CompilerError(
                            "Only named fields are supported in struct assignment patterns",
                            resolveSourceLoc(*item->key, sm));
                    }
                    std::string name(item->key->as<IdentifierNameSyntax>().identifier.valueText());
                    if (!expressions.emplace(name, item->expr).second) {
                        throw CompilerError(
                            "Duplicate field in struct assignment pattern: " + name,
                            resolveSourceLoc(*item->key, sm));
                    }
                }
                std::map<std::string, ConstantValue> values;
                for (const auto& field : expectedType.structInfo().fields) {
                    auto it = expressions.find(field.name);
                    const ExpressionSyntax* fieldExpr =
                        it != expressions.end() ? it->second : defaultExpr;
                    if (!fieldExpr) {
                        throw CompilerError(
                            "Struct assignment pattern is missing field: " + field.name,
                            resolveSourceLoc(*expr, sm));
                    }
                    values.emplace(
                        field.name,
                        evaluateConstantValue(fieldExpr, *field.type, ctx, pkgRegistry, namedTypeRegistry, sm));
                    if (it != expressions.end()) expressions.erase(it);
                }
                if (!expressions.empty()) {
                    throw CompilerError(
                        "Unknown field in struct assignment pattern: " + expressions.begin()->first,
                        resolveSourceLoc(*expr, sm));
                }
                return ConstantValue::namedStruct(expectedType, std::move(values));
            }
            throw CompilerError(
                "Replicated struct assignment patterns are not supported",
                resolveSourceLoc(*expr, sm));
        }
        case SyntaxKind::ParenthesizedExpression:
            return evaluateConstantValue(
                expr->as<ParenthesizedExpressionSyntax>().expression, expectedType, ctx, pkgRegistry, namedTypeRegistry, sm);
        default:
            break;
    }
    if (expectedType.isAggregate()) {
        throw CompilerError(
            "Unsupported aggregate package constant expression: " +
            std::string(toString(expr->kind)),
            resolveSourceLoc(*expr, sm));
    }
    return ConstantValue::bits(expectedType, evaluateConstantExpr(expr, ctx, &pkgRegistry, namedTypeRegistry));
}


// Evaluate the next genvar value from a for-loop iteration expression.
// Supports: i = expr, i++, i--, ++i, --i
int64_t evaluateStepExpr(
        const ExpressionSyntax* iterExpr,
        const std::string& loopVar,
        const ParameterContext& ctx) {
    switch (iterExpr->kind) {
        case SyntaxKind::AssignmentExpression: {
            auto& assign = iterExpr->as<BinaryExpressionSyntax>();
            return evaluateConstantExpr(assign.right, ctx);
        }
        case SyntaxKind::PostincrementExpression:
        case SyntaxKind::UnaryPreincrementExpression: {
            auto it = ctx.values.find(loopVar);
            if (it == ctx.values.end())
                throw CompilerError("Loop variable '" + loopVar + "' not found in context during increment");
            return it->second.requireInt64("Loop variable '" + loopVar + "'") + 1;
        }
        case SyntaxKind::PostdecrementExpression:
        case SyntaxKind::UnaryPredecrementExpression: {
            auto it = ctx.values.find(loopVar);
            if (it == ctx.values.end())
                throw CompilerError("Loop variable '" + loopVar + "' not found in context during decrement");
            return it->second.requireInt64("Loop variable '" + loopVar + "'") - 1;
        }
        default:
            throw CompilerError(
                "Unsupported loop step expression: " + std::string(toString(iterExpr->kind)));
    }
}

} // namespace mate
