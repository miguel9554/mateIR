#include "frontends/systemverilog/passes/io_domains_set.h"

#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace mate {

namespace {

// Classification of a port in the YAML
enum class PortClass { Clock, Reset, Sync, Async };

struct PortClassification {
    PortClass cls;
    std::string clock_name;        // only for Sync
};

std::string parseSignalRef(const YAML::Node& entry) {
    if (entry.IsScalar()) {
        return entry.as<std::string>();
    }
    if (entry.IsMap()) {
        throw CompilerError(
            "io_domains_set: synchronized_into is no longer supported in domains YAML; "
            "use <module_name>.cdc.yaml synchronizer_flops instead");
    }
    throw CompilerError("io_domains_set: invalid signal entry format (expected string)");
}

// Expand a wildcard pattern (e.g. "s_axis_*") against a set of port names.
// Returns matched names. Pattern without '*' is an exact match.
std::vector<std::string> expandPattern(
        const std::string& pattern,
        const std::set<std::string>& portNames) {
    std::vector<std::string> matches;
    if (pattern.back() == '*') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);
        for (const auto& name : portNames) {
            if (name.starts_with(prefix)) {
                matches.push_back(name);
            }
        }
    } else {
        if (portNames.contains(pattern)) {
            matches.push_back(pattern);
        }
    }
    return matches;
}

} // anonymous namespace

void setIODomains(Module& module,
                  const std::string& yamlPath,
                  FrontendDomainFacts* domainFacts,
                  InstancePath instancePath) {
    ModuleDomainFacts* privateFacts = nullptr;
    if (domainFacts) {
        privateFacts = &domainFacts->getOrCreate({instancePath, module.name});
    }

    // 3a. Load & parse YAML
    YAML::Node config = YAML::LoadFile(yamlPath);

    std::string yamlModuleName = config["module_name"].as<std::string>();
    auto clockDomainsNode = config["clock_domains"];
    auto resetsNode = config["resets"];
    auto asyncNode = config["async_domain"];

    // 3b. Validate module_name
    if (yamlModuleName != module.name) {
        throw CompilerError(std::format(
            "io_domains_set: YAML module_name '{}' does not match module '{}'",
            yamlModuleName, module.name));
    }

    // Pure combinational modules have no clock/reset domains; skip all classification.
    if (config["pure_combinational"] && config["pure_combinational"].as<bool>()) {
        module.pure_combinational = true;
        if (privateFacts) privateFacts->pure_combinational = true;
        return;
    }

    // Collect all port names
    std::set<std::string> allPortNames;
    for (const auto& [name, sig] : module.inputs) allPortNames.insert(name);
    for (const auto& [name, sig] : module.outputs) allPortNames.insert(name);

    // 3c. Build port classification
    std::map<std::string, PortClassification> portClassMap;

    // Process clock domains
    struct ClockDomainInfo {
        std::string input_port; // actual clock input port name
        std::string polarity;   // "posedge" or "negedge"
        std::vector<std::string> matched_ports;
    };
    std::map<std::string, ClockDomainInfo> clockDomains;

    if (clockDomainsNode && clockDomainsNode.IsMap()) {
        for (auto it = clockDomainsNode.begin(); it != clockDomainsNode.end(); ++it) {
            std::string domainName = it->first.as<std::string>();
            auto domainNode = it->second;

            ClockDomainInfo info;
            info.input_port = domainNode["input_name"]
                ? domainNode["input_name"].as<std::string>()
                : domainName;
            info.polarity = domainNode["polarity"].as<std::string>();
            edge_t clockEdge = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;

            // Classify the clock input itself
            if (portClassMap.contains(info.input_port)) {
                throw CompilerError(std::format(
                    "io_domains_set: module '{}': port '{}' classified in multiple domains",
                    module.name, info.input_port));
            }
            portClassMap[info.input_port] = {PortClass::Clock, domainName};

            // Expand inputs_outputs entries
            auto ioList = domainNode["inputs_outputs"];
            if (ioList && ioList.IsSequence()) {
                for (const auto& entry : ioList) {
                    auto ref = parseSignalRef(entry);
                    auto matches = expandPattern(ref, allPortNames);
                    if (matches.empty()) {
                        throw CompilerError(std::format(
                            "io_domains_set: wildcard '{}' in clock domain '{}' "
                            "matches no ports in module '{}'",
                            ref, domainName, module.name));
                    }
                    for (const auto& name : matches) {
                        if (portClassMap.contains(name)) {
                            throw CompilerError(std::format(
                                "io_domains_set: module '{}': port '{}' classified in multiple domains",
                                module.name, name));
                        }
                        portClassMap[name] = {PortClass::Sync, domainName};
                        info.matched_ports.push_back(name);
                    }
                }
            }

            clockDomains[domainName] = std::move(info);
            if (privateFacts) {
                const auto& stored = clockDomains.at(domainName);
                privateFacts->yaml_clocks[domainName] = YamlClockDomainFact{
                    .domain_name = domainName,
                    .input_port = stored.input_port,
                    .edge = clockEdge,
                    .matched_ports = stored.matched_ports,
                };
            }
        }
    }

    // Process resets
    struct ResetInfo {
        std::string signal_name; // actual reset port name
        std::string polarity;    // "positive" or "negative"
    };
    std::map<std::string, ResetInfo> resets;

    if (resetsNode && resetsNode.IsMap()) {
        for (auto it = resetsNode.begin(); it != resetsNode.end(); ++it) {
            std::string resetName = it->first.as<std::string>();
            auto resetNode = it->second;

            ResetInfo info;
            info.signal_name = resetNode["signal_name"]
                ? resetNode["signal_name"].as<std::string>()
                : resetName;
            info.polarity = resetNode["polarity"].as<std::string>();
            edge_t resetEdge = (info.polarity == "positive") ? POSEDGE : NEGEDGE;

            if (portClassMap.contains(info.signal_name)) {
                throw CompilerError(std::format(
                    "io_domains_set: module '{}': port '{}' classified in multiple domains",
                    module.name, info.signal_name));
            }
            portClassMap[info.signal_name] = {PortClass::Reset, ""};

            resets[resetName] = std::move(info);
            if (privateFacts) {
                const auto& stored = resets.at(resetName);
                privateFacts->yaml_resets[resetName] = YamlResetDomainFact{
                    .reset_name = resetName,
                    .signal_name = stored.signal_name,
                    .active_edge = resetEdge,
                };
            }
        }
    }

    // Process async_domain
    if (asyncNode && asyncNode.IsSequence()) {
        for (const auto& entry : asyncNode) {
            auto ref = parseSignalRef(entry);
            auto matches = expandPattern(ref, allPortNames);
            if (matches.empty()) {
                throw CompilerError(std::format(
                    "io_domains_set: wildcard '{}' in async_domain "
                    "matches no ports in module '{}'",
                    ref, module.name));
            }
            for (const auto& name : matches) {
                if (portClassMap.contains(name)) {
                    throw CompilerError(std::format(
                        "io_domains_set: module '{}': port '{}' classified in multiple domains",
                        module.name, name));
                }
                portClassMap[name] = {PortClass::Async, ""};
            }
        }
    }

    // 3d. Structural validation

    // Every classified port must exist in module inputs or outputs
    for (const auto& [portName, cls] : portClassMap) {
        if (!allPortNames.contains(portName)) {
            throw CompilerError(std::format(
                "io_domains_set: classified port '{}' does not exist in module '{}'",
                portName, module.name));
        }
    }

    // Clock input ports must be inputs
    for (const auto& [domainName, info] : clockDomains) {
        if (!module.inputs.contains(info.input_port)) {
            throw CompilerError(std::format(
                "io_domains_set: module '{}': clock '{}' (port '{}') is not a module input",
                module.name, domainName, info.input_port));
        }
    }

    // Reset input ports must be inputs
    for (const auto& [resetName, info] : resets) {
        if (!module.inputs.contains(info.signal_name)) {
            throw CompilerError(std::format(
                "io_domains_set: module '{}': reset '{}' (port '{}') is not a module input",
                module.name, resetName, info.signal_name));
        }
    }

    // Every non-clock, non-reset I/O must be classified
    for (const auto& portName : allPortNames) {
        if (!portClassMap.contains(portName)) {
            throw CompilerError(std::format(
                "io_domains_set: port '{}' in module '{}' is not classified "
                "in any clock domain, reset, or async_domain",
                portName, module.name));
        }
    }

    // Store frontend-private local classification facts. Final public IR sync state
    // is assigned later by domains_propagate_and_check after global domains resolve.
    for (const auto& [portName, cls] : portClassMap) {
        if (privateFacts) {
            LocalPortClass factClass = LocalPortClass::Sync;
            switch (cls.cls) {
                case PortClass::Clock: factClass = LocalPortClass::Clock; break;
                case PortClass::Reset: factClass = LocalPortClass::Reset; break;
                case PortClass::Async: factClass = LocalPortClass::Async; break;
                case PortClass::Sync: factClass = LocalPortClass::Sync; break;
            }
            privateFacts->ports[portName] = LocalPortDomainFact{
                .port_name = portName,
                .cls = factClass,
                .local_domain_name = cls.cls == PortClass::Sync
                    ? std::optional<std::string>(cls.clock_name)
                    : std::nullopt,
                .edge = std::nullopt,
            };
        }
    }

    // Store each clock's expected edge in frontend-private facts so later passes can
    // validate polarity without re-parsing YAML.
    for (const auto& [domainName, info] : clockDomains) {
        edge_t edge = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;

        for (const auto& portName : info.matched_ports) {
            if (privateFacts) {
                auto portIt = privateFacts->ports.find(portName);
                if (portIt != privateFacts->ports.end()) portIt->second.edge = edge;
            }
        }
        if (privateFacts) {
            auto portIt = privateFacts->ports.find(info.input_port);
            if (portIt != privateFacts->ports.end()) portIt->second.edge = edge;
        }
    }

    // Store each reset's expected polarity in frontend-private facts.
    for (const auto& [resetName, info] : resets) {
        edge_t edge = (info.polarity == "positive") ? POSEDGE : NEGEDGE;
        if (privateFacts) {
            auto portIt = privateFacts->ports.find(info.signal_name);
            if (portIt != privateFacts->ports.end()) portIt->second.edge = edge;
        }
    }
}

} // namespace mate
