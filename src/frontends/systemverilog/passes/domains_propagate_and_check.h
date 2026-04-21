#pragma once

#include "mateir/module.h"

namespace mate {

// Pass 12: run after flop_resolve and dce.
// - Validates clock/reset polarity (YAML edge vs. flop_resolve edge)
// - Validates clock/reset sync_kind on flop trigger signals
// - Propagates clock_domain/clock_edge to internal signals and flop types
// - CDC fanin validation: checks sync/async inputs only reach flops in their domain
void domainsPropagateAndCheck(Module& module);

} // namespace mate
