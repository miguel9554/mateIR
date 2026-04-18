#pragma once

#include "frontends/frontend.h"

namespace custom_hdl {

class SystemVerilogFrontend final : public Frontend {
public:
    std::string name() const override;
    MateIR compile(const FrontendOptions& options) const override;
};

} // namespace custom_hdl
