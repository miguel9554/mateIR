#include "consumers/sim/simulator_consumer.h"

namespace custom_hdl {

SimulatorConsumer::SimulatorConsumer(SimConfig config)
    : config_(std::move(config)) {}

std::string SimulatorConsumer::name() const {
    return "simulator";
}

void SimulatorConsumer::consume(const MateIR& ir) {
    Simulator sim(ir.top, config_);
    sim.run();
}

} // namespace custom_hdl
