#pragma once

#include "consumers/ir_consumer.h"
#include "mateir/debug.h"

#include <cstddef>
#include <iosfwd>
#include <vector>

namespace mate {

struct StaticAnalysisSummary {
    size_t modules = 0;
    size_t inputs = 0;
    size_t outputs = 0;
    size_t signals = 0;
    size_t flops = 0;
    size_t dfg_nodes = 0;
};

class StaticAnalysisConsumer final : public IRConsumer {
public:
    explicit StaticAnalysisConsumer(std::ostream& out,
                                    std::vector<DebugNodeSpec> debug_dfg_node_deps = {});

    std::string name() const override;
    void consume(const MateIR& ir) override;

private:
    std::ostream& out_;
    std::vector<DebugNodeSpec> debug_dfg_node_deps_;
};

StaticAnalysisSummary analyzeMateIR(const MateIR& ir);

} // namespace mate
