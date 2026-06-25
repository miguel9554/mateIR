#include "consumers/sim/simulator_consumer.h"

namespace mate {

SimulatorConsumer::SimulatorConsumer(SimConfig config)
    : config_(std::move(config)) {}

std::string SimulatorConsumer::name() const {
    return "simulator";
}

void SimulatorConsumer::consume(const RtlRuntimeModel& model) {
    Simulator sim(model, config_);
    sim.run();
}

} // namespace mate
