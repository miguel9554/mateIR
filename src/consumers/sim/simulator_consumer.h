#pragma once

#include "consumers/ir_consumer.h"
#include "sim/simulator.h"

namespace mate {

class SimulatorConsumer final : public IRConsumer {
public:
    explicit SimulatorConsumer(SimConfig config);

    std::string name() const override;
    void consume(const MateIR& ir) override;

private:
    SimConfig config_;
};

} // namespace mate
