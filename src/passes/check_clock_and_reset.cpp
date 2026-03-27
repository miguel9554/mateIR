#include "passes/check_clock_and_reset.h"

#include "util/source_loc.h"

#include <format>
#include <functional>

namespace custom_hdl {

namespace {

void checkModule(const ResolvedModule& mod) {
    for (const auto& flop : mod.flops) {
        const std::string& clk_name = flop.clock.name;
        auto clk_it = mod.inputs.find(clk_name);
        if (clk_it != mod.inputs.end() &&
            clk_it->second.sync_kind != SyncKind::Clock) {
            throw CompilerError(std::format(
                "check_clock_and_reset: flop '{}' in module '{}': clock signal '{}' "
                "is not declared as a clock in the domains file",
                flop.name, mod.name, clk_name));
        }

        if (flop.reset) {
            const std::string& rst_name = flop.reset->name;
            auto rst_it = mod.inputs.find(rst_name);
            if (rst_it != mod.inputs.end() &&
                rst_it->second.sync_kind != SyncKind::Reset) {
                throw CompilerError(std::format(
                    "check_clock_and_reset: flop '{}' in module '{}': reset signal '{}' "
                    "is not declared as a reset in the domains file",
                    flop.name, mod.name, rst_name));
            }
        }
    }
}

} // anonymous namespace

void checkClockAndReset(const ResolvedModule& module) {
    checkModule(module);
    for (const auto& sub : module.hierarchyInstantiation)
        checkClockAndReset(sub);
}

} // namespace custom_hdl
