#pragma once

#include "mateir/dfg.h"
#include "util/bit_vector_value.h"

#include <functional>
#include <optional>

namespace mate {

// Reference semantics of DFG nodes.
//
// This is the single compile-time definition of what a DFG node computes, bit
// for bit. It mirrors exactly what the generated native model computes for
// the same node (dpi_codegen.cpp makeNativeCombinationalCpp, over
// FixedValue): every operation follows the FixedValue method the codegen
// emits, and the result is resized to the node's own type. BitVectorValue and
// FixedValue implement the same primitives and are cross-checked by
// tests/unit/fixed_value_diff_test.cpp.
//
// Any compile-time rewrite that replaces a node by a computed value
// (constant_fold) must evaluate through here, so that it can never change
// what the generated model observes. When per-op lowering in dpi_codegen
// changes, this file changes with it.

// True for types these functions evaluate: a plain bit vector (integer or
// enum) with no unpacked dimensions and positive width.
bool isEvaluableBitVectorType(const Type& type);

// Value of a CONST node as the generated model materializes it
// (FixedValue::fromI64): the payload's two's-complement bits in the low word,
// zero above bit 63, truncated to the node's width.
BitVectorValue constNodeValue(const DFGNode& node);

using DFGOperandValueFn = std::function<BitVectorValue(const DFGNode*)>;

// Value computed by `node` from its operands' values, resized to the node's
// own type. `operandValue` must return each operand's value at that
// operand's type. CONST evaluates to constNodeValue. INPUT and X have no
// defining computation and throw, as does any node or operand whose type is
// missing or not an evaluable bit vector.
BitVectorValue evaluateDFGNode(const DFGNode& node, const DFGOperandValueFn& operandValue);

// CONST payload that constNodeValue decodes back to exactly `value` at
// `type`, or nullopt when `value` has bits set at or above bit 64 (the
// payload cannot hold them). Signed types narrower than 64 bits use the
// sign-extended payload form; everything else stores the raw low word.
std::optional<int64_t> constPayloadFor(const BitVectorValue& value, const Type& type);

} // namespace mate
