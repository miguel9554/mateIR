#pragma once

#include "ir/resolved.h"

namespace custom_hdl {

// Resolve flop clock/reset structure from the DFG.
// This pass extracts the reset MUX pattern from each flop's .d driver,
// identifies clock and reset signals, validates that functional logic
// doesn't use clock/reset, and strips the reset MUX leaving only
// functional logic driving .d.
//
// Must run after constant folding and condition normalization so that
// the reset MUX pattern is in canonical form.
void resolveFlops(ResolvedModule& module);

// Propagate Clock/Reset type tags from submodule inputs up to parent
// inputs through MODULE node connections. Must run after resolveFlops
// (bottom-up) so submodule input types are already resolved.
void propagatePortTypes(ResolvedModule& module);

} // namespace custom_hdl
