#include "consumers/static_analysis/static_analysis.h"

#include <algorithm>
#include <format>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace mate {

namespace {

const char* edgeName(edge_t edge) {
    switch (edge) {
        case POSEDGE: return "posedge";
        case NEGEDGE: return "negedge";
    }
    return "unknown";
}

const char* signalNamespaceName(SignalNamespace ns) {
    switch (ns) {
        case SignalNamespace::Input: return "input";
        case SignalNamespace::Output: return "output";
        case SignalNamespace::Internal: return "internal";
        case SignalNamespace::FlopQ: return "flop_q";
        case SignalNamespace::FlopD: return "flop_d";
    }
    return "unknown";
}

std::string instancePathName(const InstancePath& path) {
    if (path.elems.empty()) return "<top>";
    std::string result;
    for (size_t i = 0; i < path.elems.size(); ++i) {
        if (i) result += ".";
        result += path.elems[i];
    }
    return result;
}

std::string hierSignalRefName(const HierSignalRef& ref) {
    return std::format("{} {}.{}",
                       signalNamespaceName(ref.ns),
                       instancePathName(ref.instance_path),
                       ref.name);
}

const ClockDomain* findClockDomain(const MateIR& ir, ClockId id) {
    auto it = std::find_if(ir.clocks.begin(), ir.clocks.end(), [&](const ClockDomain& domain) {
        return domain.id == id;
    });
    return it == ir.clocks.end() ? nullptr : &*it;
}

const ResetDomain* findResetDomain(const MateIR& ir, ResetId id) {
    auto it = std::find_if(ir.resets.begin(), ir.resets.end(), [&](const ResetDomain& domain) {
        return domain.id == id;
    });
    return it == ir.resets.end() ? nullptr : &*it;
}

std::string clockDomainLabel(const MateIR& ir, ClockId id) {
    if (id == InvalidClockId) return "invalid";
    if (const auto* domain = findClockDomain(ir, id)) {
        return std::format("({}, {})",
                           domain->display_name,
                           edgeName(domain->edge));
    }
    return "unknown";
}

std::string resetDomainLabel(const MateIR& ir, ResetId id) {
    if (id == InvalidResetId) return "invalid";
    if (const auto* domain = findResetDomain(ir, id)) {
        return std::format("({}, {})",
                           domain->display_name,
                           edgeName(domain->active_edge));
    }
    return "unknown";
}

std::string moduleDisplayName(const Module& module) {
    if (module.instance_name.empty()) return module.name;
    return std::format("{} ({})", module.instance_name, module.name);
}

void collectModule(const Module& module, StaticAnalysisSummary& summary) {
    summary.modules++;
    forEachInputNode(module, [&](const ModuleNode&) { summary.inputs++; });
    forEachOutputNode(module, [&](const ModuleNode&) { summary.outputs++; });
    forEachInternalNode(module, [&](const ModuleNode&) { summary.signals++; });
    summary.flops += module.flops.size();
    if (module.dfg) summary.dfg_nodes += module.dfg->nodes.size();
    validateNoCombLoops(module);
    for (const auto& sub : module.hierarchyInstantiation)
        collectModule(sub, summary);
}

void printDomainRegistry(const MateIR& ir, std::ostream& out) {
    out << "  clock_domains:\n";
    if (ir.clocks.empty()) {
        out << "    none\n";
    } else {
        for (const auto& clock : ir.clocks) {
            out << "    " << clock.id.value
                << ": " << clock.display_name
                << " edge=" << edgeName(clock.edge)
                << " source=" << hierSignalRefName(clock.source)
                << "\n";
        }
    }

    out << "  reset_domains:\n";
    if (ir.resets.empty()) {
        out << "    none\n";
    } else {
        for (const auto& reset : ir.resets) {
            out << "    " << reset.id.value
                << ": " << reset.display_name
                << " active_edge=" << edgeName(reset.active_edge)
                << " source=" << hierSignalRefName(reset.source)
                << "\n";
        }
    }
}

void printDomainSet(const MateIR& ir,
                    const std::set<ClockId>& clocks,
                    std::ostream& out,
                    const std::string& indent) {
    if (clocks.empty()) return;

    out << indent << "clock_domains:\n";
    for (ClockId clock : clocks)
        out << indent << "\t" << clockDomainLabel(ir, clock) << "\n";
}

void printDomainSet(const MateIR& ir,
                    const std::set<ResetId>& resets,
                    std::ostream& out,
                    const std::string& indent) {
    if (resets.empty()) return;

    out << indent << "reset_domains:\n";
    for (ResetId reset : resets)
        out << indent << "\t" << resetDomainLabel(ir, reset) << "\n";
}

void printModuleDomains(const MateIR& ir,
                        const Module& module,
                        std::ostream& out,
                        size_t depth = 0) {
    std::string indent(depth + 1, '\t');
    std::string childIndent(depth + 2, '\t');
    out << indent << "module " << moduleDisplayName(module) << ":\n";

    std::set<ClockId> clocks;
    std::set<ResetId> resets;
    for (const auto& flop : module.flops) {
        if (flop.clock_domain != InvalidClockId)
            clocks.insert(flop.clock_domain);
        for (ResetId reset : flop.reset_domains.ids) {
            if (reset != InvalidResetId)
                resets.insert(reset);
        }
    }

    printDomainSet(ir, clocks, out, childIndent);
    printDomainSet(ir, resets, out, childIndent);

    for (const auto& sub : module.hierarchyInstantiation) {
        printModuleDomains(ir, sub, out, depth + 1);
    }
}

} // namespace

StaticAnalysisConsumer::StaticAnalysisConsumer(std::ostream& out)
    : out_(out) {}

std::string StaticAnalysisConsumer::name() const {
    return "static-analysis";
}

void StaticAnalysisConsumer::consume(const MateIR& ir) {
    auto summary = analyzeMateIR(ir);
    out_ << "mateir static analysis\n";
    out_ << "  top: " << ir.top.name << "\n";
    out_ << "  modules: " << summary.modules << "\n";
    out_ << "  inputs: " << summary.inputs << "\n";
    out_ << "  outputs: " << summary.outputs << "\n";
    out_ << "  signals: " << summary.signals << "\n";
    out_ << "  flops: " << summary.flops << "\n";
    out_ << "  dfg_nodes: "              << summary.dfg_nodes     << "\n";
    printDomainRegistry(ir, out_);
    out_ << "  domain_usage:\n";
    printModuleDomains(ir, ir.top, out_);
}

StaticAnalysisSummary analyzeMateIR(const MateIR& ir) {
    StaticAnalysisSummary summary;
    collectModule(ir.top, summary);
    return summary;
}

} // namespace mate
