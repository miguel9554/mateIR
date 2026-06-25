#pragma once

#include "sim/simulator.h"

namespace mate {

class SimulatorConsumer final {
public:
    explicit SimulatorConsumer(SimConfig config);

    std::string name() const;
    void consume(const RtlRuntimeModel& model);

private:
    SimConfig config_;
};

} // namespace mate
