#include "consumers/static_analysis/static_analysis.h"

#include <iostream>

namespace mate {

namespace {

void collectModule(const Module& module, StaticAnalysisSummary& summary) {
    summary.modules++;
    summary.inputs += module.inputs.size();
    summary.outputs += module.outputs.size();
    summary.signals += module.signals.size();
    summary.flops += module.flops.size();
    if (module.dfg) summary.dfg_nodes += module.dfg->nodes.size();
    validateNoCombLoops(module);
    for (const auto& sub : module.hierarchyInstantiation)
        collectModule(sub, summary);
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
}

StaticAnalysisSummary analyzeMateIR(const MateIR& ir) {
    StaticAnalysisSummary summary;
    collectModule(ir.top, summary);
    return summary;
}

} // namespace mate
