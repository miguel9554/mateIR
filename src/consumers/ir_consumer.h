#pragma once

#include "mateir/mateir.h"

#include <string>

namespace mate {

class IRConsumer {
public:
    virtual ~IRConsumer() = default;
    virtual std::string name() const = 0;
    virtual void consume(const MateIR& ir) = 0;
};

} // namespace mate
