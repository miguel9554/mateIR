#pragma once

#include "ir/resolved.h"

namespace custom_hdl {

// Pass 12: verify that every flop's clock and reset signals carry the correct
// SyncKind (Clock / Reset) as set by io_domains_set. Throws if a signal is
// present as an input port but not declared with the right kind in the domains file.
void checkClockAndReset(const ResolvedModule& module);

} // namespace custom_hdl
