#pragma once

#include "mateir/dfg.h"

namespace custom_hdl {

// Reorder CONCAT inputs by descending high index (MSB first) and
// unwrap CONCAT_ALIGN temporaries, connecting directly to the expression nodes.
// Orphaned CONCAT_ALIGN nodes are left for subsequent DCE.
// Returns true if any changes were made.
bool cleanupConcats(DFG& graph);

} // namespace custom_hdl
