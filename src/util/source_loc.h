#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace mate {

struct SourceLoc {
    std::string file;
    size_t line;
    size_t column;

    std::string str() const {
        return file + ":" + std::to_string(line) + ":" + std::to_string(column);
    }
};

struct DFGNode; // forward declare for errorNode

class CompilerError : public std::runtime_error {
public:
    std::optional<SourceLoc> loc;
    const DFGNode* errorNode = nullptr;

    explicit CompilerError(const std::string& msg,
                           std::optional<SourceLoc> loc = std::nullopt)
        : std::runtime_error(loc ? loc->str() + ": " + msg : msg),
          loc(std::move(loc)) {}

    CompilerError(const std::string& msg, const DFGNode* node);
};

} // namespace mate
