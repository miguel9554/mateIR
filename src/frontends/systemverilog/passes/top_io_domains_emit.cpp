#include "frontends/systemverilog/passes/top_io_domains_emit.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace mate {

namespace {

const char* clockPolarityName(edge_t edge) {
    return edge == POSEDGE ? "posedge" : "negedge";
}

const char* resetPolarityName(edge_t edge) {
    return edge == POSEDGE ? "positive" : "negative";
}

std::vector<std::string> topInputOrder(const Module& module) {
    std::vector<std::string> names;
    forEachInputNode(module, [&](const ModuleNode& input) {
        names.push_back(input.name);
    });
    return names;
}

template<typename Fact, typename Fn>
std::vector<std::string> orderedNamesByTopPort(
        const std::vector<std::string>& portOrder,
        const std::map<std::string, Fact>& facts,
        Fn portNameFn) {
    std::vector<std::string> orderedNames;
    std::set<std::string> used;

    for (const auto& portName : portOrder) {
        for (const auto& [name, fact] : facts) {
            if (used.contains(name) || portNameFn(fact) != portName) continue;
            orderedNames.push_back(name);
            used.insert(name);
        }
    }

    for (const auto& [name, fact] : facts) {
        (void)fact;
        if (!used.contains(name)) orderedNames.push_back(name);
    }
    return orderedNames;
}

std::vector<std::string> orderedSyncInputsForDomain(
        const std::vector<std::string>& portOrder,
        const TopInputDomainFacts& topInputs,
        const std::string& domainName) {
    std::vector<std::string> syncInputs;
    std::set<std::string> seen;
    for (const auto& portName : portOrder) {
        auto it = topInputs.sync_inputs.find(portName);
        if (it == topInputs.sync_inputs.end()) continue;
        if (it->second.clock_domain_name != domainName) continue;
        syncInputs.push_back(portName);
        seen.insert(portName);
    }
    for (const auto& [portName, fact] : topInputs.sync_inputs) {
        if (fact.clock_domain_name != domainName || seen.contains(portName)) continue;
        syncInputs.push_back(portName);
    }
    return syncInputs;
}

std::vector<std::string> orderedAsyncInputs(
        const std::vector<std::string>& portOrder,
        const TopInputDomainFacts& topInputs) {
    std::vector<std::string> asyncInputs;
    std::set<std::string> seen;
    for (const auto& portName : portOrder) {
        if (!topInputs.async_inputs.contains(portName)) continue;
        asyncInputs.push_back(portName);
        seen.insert(portName);
    }
    for (const auto& portName : topInputs.async_inputs) {
        if (!seen.contains(portName)) asyncInputs.push_back(portName);
    }
    return asyncInputs;
}

void validateTopInputFactsForEmission(const Module& module,
                                      const FrontendDomainFacts& domainFacts) {
    if (!domainFacts.top_inputs) {
        throw CompilerError("top_io_domains_emit: missing top input facts");
    }

    const auto& topInputs = *domainFacts.top_inputs;
    forEachInputNode(module, [&](const ModuleNode& input) {
        const bool isClock = std::ranges::any_of(
            topInputs.clocks, [&](const auto& entry) { return entry.second.input_port == input.name; });
        const bool isReset = std::ranges::any_of(
            topInputs.resets, [&](const auto& entry) { return entry.second.signal_name == input.name; });
        const bool isSync = topInputs.sync_inputs.contains(input.name);
        const bool isAsync = topInputs.async_inputs.contains(input.name);

        const int classifications =
            static_cast<int>(isClock) +
            static_cast<int>(isReset) +
            static_cast<int>(isSync) +
            static_cast<int>(isAsync);
        if (classifications != 1) {
            throw CompilerError(std::format(
                "top_io_domains_emit: top input '{}' in module '{}' does not have exactly one "
                "domain classification",
                input.name, module.name));
        }
    });

    for (const auto& [domainName, clock] : topInputs.clocks) {
        (void)clock;
        if (!topInputs.resolved_clocks.contains(domainName)) {
            throw CompilerError(std::format(
                "top_io_domains_emit: unresolved top clock domain '{}'", domainName));
        }
    }

    for (const auto& [resetName, reset] : topInputs.resets) {
        (void)reset;
        if (!topInputs.resolved_resets.contains(resetName)) {
            throw CompilerError(std::format(
                "top_io_domains_emit: unresolved top reset domain '{}'", resetName));
        }
    }

    for (const auto& [portName, syncFact] : topInputs.sync_inputs) {
        (void)portName;
        if (!topInputs.clocks.contains(syncFact.clock_domain_name)) {
            throw CompilerError(std::format(
                "top_io_domains_emit: sync input '{}' references unknown clock domain '{}'",
                syncFact.port_name, syncFact.clock_domain_name));
        }
    }
}

} // namespace

void writeAtomically(const std::string& outputPath, const std::string& contents) {
    namespace fs = std::filesystem;

    fs::path finalPath(outputPath);
    if (finalPath.has_parent_path()) {
        fs::create_directories(finalPath.parent_path());
    }

    fs::path tempPath = finalPath;
    tempPath += ".tmp";

    {
        std::ofstream out(tempPath);
        if (!out) {
            throw CompilerError(std::format(
                "top_io_domains_emit: failed to open '{}' for writing", tempPath.string()));
        }
        out << contents;
        if (!out) {
            throw CompilerError(std::format(
                "top_io_domains_emit: failed to write '{}'", tempPath.string()));
        }
    }

    std::error_code ec;
    fs::rename(tempPath, finalPath, ec);
    if (ec) {
        fs::remove(finalPath, ec);
        ec.clear();
        fs::rename(tempPath, finalPath, ec);
    }
    if (ec) {
        fs::remove(tempPath);
        throw CompilerError(std::format(
            "top_io_domains_emit: failed to move '{}' into place at '{}': {}",
            tempPath.string(), finalPath.string(), ec.message()));
    }
}

void emitInferredTopDomainsYaml(const Module& module,
                                const FrontendDomainFacts& domainFacts,
                                const std::string& outputPath) {
    validateTopInputFactsForEmission(module, domainFacts);
    const auto& topInputs = *domainFacts.top_inputs;
    const auto portOrder = topInputOrder(module);

    const auto orderedClockNames = orderedNamesByTopPort(
        portOrder, topInputs.clocks, [](const TopClockInputFact& fact) { return fact.input_port; });
    const auto orderedResetNames = orderedNamesByTopPort(
        portOrder, topInputs.resets, [](const TopResetInputFact& fact) { return fact.signal_name; });
    const auto orderedAsyncInputNames = orderedAsyncInputs(portOrder, topInputs);

    std::ostringstream yaml;
    yaml << "module_name: " << module.name << "\n";

    if (!orderedResetNames.empty()) {
        yaml << "resets:\n";
        for (const auto& resetName : orderedResetNames) {
            const auto& reset = topInputs.resets.at(resetName);
            yaml << "  " << resetName << ":\n";
            if (reset.signal_name != resetName) {
                yaml << "    signal_name: " << reset.signal_name << "\n";
            }
            yaml << "    polarity: " << resetPolarityName(reset.active_edge) << "\n";
        }
    }

    if (!orderedClockNames.empty()) {
        yaml << "clock_domains:\n";
        for (const auto& domainName : orderedClockNames) {
            const auto& clock = topInputs.clocks.at(domainName);
            yaml << "  " << domainName << ":\n";
            if (clock.input_port != domainName) {
                yaml << "    input_name: " << clock.input_port << "\n";
            }
            yaml << "    polarity: " << clockPolarityName(clock.edge) << "\n";
            yaml << "    inputs_outputs:\n";
            for (const auto& portName : orderedSyncInputsForDomain(portOrder, topInputs, domainName)) {
                yaml << "      - " << portName << "\n";
            }
        }
    }

    if (!orderedAsyncInputNames.empty()) {
        yaml << "async_domain:\n";
        for (const auto& portName : orderedAsyncInputNames) {
            yaml << "  - " << portName << "\n";
        }
    }

    writeAtomically(outputPath, yaml.str());
}

} // namespace mate
