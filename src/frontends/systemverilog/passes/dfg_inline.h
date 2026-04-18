#pragma once

#include "mateir/module.h"

namespace custom_hdl {

// Inline all submodule DFGs into the top-level DFG.
// Must run after all modules are elaborated (bottom-up).
// After this pass:
//   - No MODULE nodes exist in top.dfg
//   - All submodule computation is merged into top.dfg
//   - Flop .d OUTPUT nodes from submodules are in top.dfg's outputs map
//   - Flop .q INPUT nodes from submodules are in top.dfg's inputs map
//   - DFGNode::instance_path is set on all inlined nodes
//   - Submodule dfg fields are nulled
void inlineDFGs(Module& top);

} // namespace custom_hdl
