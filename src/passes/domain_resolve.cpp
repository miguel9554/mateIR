#include "passes/domain_resolve.h"

#include <format>

namespace custom_hdl {

void resolveDomains(ResolvedModule& module) {
    // Find the single clock among inputs
    ResolvedSignal* clock = nullptr;
    for (auto& input : module.inputs) {
        if (input.type.kind == ResolvedTypeKind::Clock) {
            if (clock) {
                throw CompilerError(std::format(
                    "Module '{}': multiple clocks found ('{}' and '{}'), "
                    "only single-clock-domain modules are supported",
                    module.name, clock->name, input.name));
            }
            clock = &input;
        }
    }

    // Modules without flops have no clock-tagged inputs; nothing to resolve
    if (!clock) {
        if (!module.flops.empty()) {
            throw CompilerError(std::format(
                "Module '{}': has flops but no clock found among inputs", module.name));
        }
        return;
    }

    // Determine the clock's active edge from the flops
    if (module.flops.empty()) {
        throw CompilerError(std::format(
            "Module '{}': has clock '{}' but no flops", module.name, clock->name));
    }
    edge_t clock_edge = module.flops.front().clock.edge;

    // Set clock_domain and clock_edge for all non-Clock, non-Reset signals
    auto assign = [&](ResolvedSignal& sig) {
        if (sig.type.kind != ResolvedTypeKind::Clock &&
            sig.type.kind != ResolvedTypeKind::Reset) {
            sig.clock_domain = clock;
            sig.clock_edge = clock_edge;
        }
    };

    for (auto& sig : module.inputs)  assign(sig);
    for (auto& sig : module.outputs) assign(sig);
    for (auto& sig : module.signals) assign(sig);
    for (auto& flop : module.flops)  assign(flop.type);
}

} // namespace custom_hdl
