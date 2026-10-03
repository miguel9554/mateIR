#include "mateir/dfg_eval.h"

#include <format>
#include <vector>

namespace mate {

namespace {

const Type& evaluableType(const DFGNode* node) {
    if (!node->type) {
        throw CompilerError(std::format(
            "evaluateDFGNode: node {} has no type", node->str()), node);
    }
    if (!isEvaluableBitVectorType(*node->type)) {
        throw CompilerError(std::format(
            "evaluateDFGNode: node {} type is not an evaluable bit vector", node->str()), node);
    }
    return *node->type;
}

BitVectorValue operand(const DFGNode* node, const DFGOperandValueFn& operandValue) {
    const Type& type = evaluableType(node);
    BitVectorValue value = operandValue(node);
    if (value.isAggregate() || value.width() != type.width ||
        value.isSigned() != type.isSigned()) {
        throw CompilerError(std::format(
            "evaluateDFGNode: operand value for {} does not match its type", node->str()), node);
    }
    return value;
}

BitVectorValue bit(bool value) {
    return BitVectorValue::fromU64(value ? 1 : 0, 1, false);
}

// Mirrors dpi_codegen compareIsSigned: an operation is signed only when both
// operands are.
bool bothSigned(const DFGNode* lhs, const DFGNode* rhs) {
    return evaluableType(lhs).isSigned() && evaluableType(rhs).isSigned();
}

// Result of `node`'s own operation before the final resize to its type.
BitVectorValue evaluateOp(const DFGNode& node, const DFGOperandValueFn& operandValue) {
    auto value = [&](const DFGNode* n) { return operand(n, operandValue); };
    const DFGOp kind = node.kind();
    switch (kind) {
        case DFGOp::CONST:
            return constNodeValue(node);

        case DFGOp::INPUT:
        case DFGOp::X:
            throw CompilerError(std::format(
                "evaluateDFGNode: {} has no defining computation", node.str()), &node);

        case DFGOp::SIGNAL:
        case DFGOp::OUTPUT: {
            auto driver = node.driver();
            if (!driver) {
                throw CompilerError(std::format(
                    "evaluateDFGNode: driven node {} has no driver", node.str()), &node);
            }
            return value(driver->node);
        }

        case DFGOp::ADD:
        case DFGOp::SUB:
        case DFGOp::MUL: {
            const auto inputs = node.binaryInputs();
            const int width = node.type->width;
            const bool is_signed = bothSigned(inputs.lhs.node, inputs.rhs.node);
            const BitVectorValue lhs = value(inputs.lhs.node).resized(width, is_signed);
            const BitVectorValue rhs = value(inputs.rhs.node).resized(width, is_signed);
            if (kind == DFGOp::ADD) return lhs.add(rhs);
            if (kind == DFGOp::SUB) return lhs.sub(rhs);
            return lhs.mul(rhs);
        }

        case DFGOp::EQ: {
            const auto inputs = node.binaryInputs();
            return bit(value(inputs.lhs.node).eq(value(inputs.rhs.node)));
        }

        case DFGOp::LT:
        case DFGOp::LE:
        case DFGOp::GT:
        case DFGOp::GE: {
            const auto inputs = node.binaryInputs();
            const BitVectorValue lhs = value(inputs.lhs.node);
            const BitVectorValue rhs = value(inputs.rhs.node);
            const bool is_signed = bothSigned(inputs.lhs.node, inputs.rhs.node);
            auto less = [&](const BitVectorValue& a, const BitVectorValue& b) {
                return is_signed ? a.signedLt(b) : a.unsignedLt(b);
            };
            if (kind == DFGOp::LT) return bit(less(lhs, rhs));
            if (kind == DFGOp::LE) return bit(less(lhs, rhs) || lhs.eq(rhs));
            if (kind == DFGOp::GT) return bit(less(rhs, lhs));
            return bit(!less(lhs, rhs));
        }

        case DFGOp::SHL:
        case DFGOp::SHR:
        case DFGOp::ASR: {
            const auto inputs = node.binaryInputs();
            const BitVectorValue lhs = value(inputs.lhs.node);
            const uint64_t amount = value(inputs.rhs.node).lowU64();
            if (kind == DFGOp::SHL) return lhs.shl(amount);
            return lhs.shr(amount, kind == DFGOp::ASR);
        }

        case DFGOp::MUX: {
            // The generated switch compares the selector's low word, read as
            // int64, against the arm codes; an unmatched selector throws.
            const int64_t code = static_cast<int64_t>(value(node.muxSelector().node).lowU64());
            const int arm = node.muxArmIndexForValue(code);
            if (arm < 0) {
                throw CompilerError(std::format(
                    "evaluateDFGNode: MUX {} has no arm for selector value {}", node.str(), code),
                    &node);
            }
            return value(node.muxArmData(static_cast<size_t>(arm)).node);
        }

        case DFGOp::UNARY_NEGATE:
            return value(node.unaryInputs().operand.node).negated();
        case DFGOp::BITWISE_NOT:
            return value(node.unaryInputs().operand.node).bitwiseNot();

        case DFGOp::BITWISE_AND:
        case DFGOp::BITWISE_OR:
        case DFGOp::BITWISE_XOR:
        case DFGOp::BITWISE_XNOR: {
            const auto inputs = node.binaryInputs();
            const BitVectorValue lhs = value(inputs.lhs.node);
            const BitVectorValue rhs = value(inputs.rhs.node);
            if (kind == DFGOp::BITWISE_AND) return lhs.bitwiseAnd(rhs);
            if (kind == DFGOp::BITWISE_OR) return lhs.bitwiseOr(rhs);
            if (kind == DFGOp::BITWISE_XOR) return lhs.bitwiseXor(rhs);
            return lhs.bitwiseXnor(rhs);
        }

        case DFGOp::REDUCTION_AND:
            return bit(value(node.unaryInputs().operand.node).reductionAnd());
        case DFGOp::REDUCTION_NAND:
            return bit(!value(node.unaryInputs().operand.node).reductionAnd());
        case DFGOp::REDUCTION_OR:
            return bit(value(node.unaryInputs().operand.node).reductionOr());
        case DFGOp::REDUCTION_NOR:
            return bit(!value(node.unaryInputs().operand.node).reductionOr());
        case DFGOp::REDUCTION_XOR:
            return bit(value(node.unaryInputs().operand.node).reductionXor());
        case DFGOp::REDUCTION_XNOR:
            return bit(!value(node.unaryInputs().operand.node).reductionXor());

        case DFGOp::SLICE: {
            // Result bit j = source bit indices[j], as an unsigned vector of
            // indices.size() bits (slice / gatherAffine / gatherBits).
            const DFGNode* source_node = node.sliceSource().node;
            const BitVectorValue source = value(source_node);
            const auto& indices = node.sliceIndices();
            BitVectorValue result = BitVectorValue::zero(static_cast<int>(indices.size()), false);
            for (size_t j = 0; j < indices.size(); ++j) {
                if (indices[j] < 0 || indices[j] >= source.width()) {
                    throw CompilerError(std::format(
                        "evaluateDFGNode: SLICE {} reads bit {} outside source width {}",
                        node.str(), indices[j], source.width()), &node);
                }
                result.setBit(static_cast<int>(j), source.getBit(static_cast<int>(indices[j])));
            }
            return result;
        }

        case DFGOp::CONCAT: {
            std::vector<BitVectorValue> parts;
            parts.reserve(node.concatParts().size());
            for (const auto& part : node.concatParts()) parts.push_back(value(part.node));
            return BitVectorValue::concat(parts);
        }
    }
    throw CompilerError(std::format("evaluateDFGNode: unhandled op in {}", node.str()), &node);
}

} // namespace

bool isEvaluableBitVectorType(const Type& type) {
    return type.width > 0 && type.unpacked_dims.empty() && !type.isStruct();
}

BitVectorValue constNodeValue(const DFGNode& node) {
    if (node.kind() != DFGOp::CONST) {
        throw CompilerError(std::format("constNodeValue: {} is not CONST", node.str()), &node);
    }
    const Type& type = evaluableType(&node);
    return BitVectorValue::fromI64(node.constValue(), type.width, type.isSigned());
}

BitVectorValue evaluateDFGNode(const DFGNode& node, const DFGOperandValueFn& operandValue) {
    const Type& type = evaluableType(&node);
    return evaluateOp(node, operandValue).resized(type.width, type.isSigned());
}

std::optional<int64_t> constPayloadFor(const BitVectorValue& value, const Type& type) {
    if (!isEvaluableBitVectorType(type) || value.isAggregate() ||
        value.width() != type.width) {
        throw CompilerError("constPayloadFor: value does not match its type");
    }
    for (int b = 64; b < type.width; ++b) {
        if (value.getBit(b)) return std::nullopt;
    }
    uint64_t raw = value.lowU64();
    if (type.isSigned() && type.width < 64 && value.getBit(type.width - 1)) {
        raw |= ~uint64_t{0} << type.width;
    }
    return static_cast<int64_t>(raw);
}

} // namespace mate
