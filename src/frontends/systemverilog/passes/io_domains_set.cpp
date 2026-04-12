#include "frontends/systemverilog/passes/io_domains_set.h"

#include <format>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace custom_hdl {

namespace {

// Classification of a port in the YAML
enum class PortClass { Clock, Reset, Sync, Async };

struct PortClassification {
    PortClass cls;
    std::string clock_name;        // only for Sync
    std::string synchronized_into; // optional: declared target domain for cross-domain crossing
};

// Parse a signal_with_attrs YAML entry.
// Either a plain string or a single-key map: { signal_name: { synchronized_into: domain } }
struct SignalRef {
    std::string name;
    std::string synchronized_into; // empty if not specified
};

SignalRef parseSignalRef(const YAML::Node& entry) {
    if (entry.IsScalar()) {
        return {entry.as<std::string>(), ""};
    }
    if (entry.IsMap() && entry.size() == 1) {
        auto it = entry.begin();
        SignalRef ref;
        ref.name = it->first.as<std::string>();
        auto attrs = it->second;
        if (attrs["synchronized_into"]) {
            ref.synchronized_into = attrs["synchronized_into"].as<std::string>();
        }
        return ref;
    }
    throw CompilerError("io_domains_set: invalid signal entry format (expected string or single-key map)");
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

// Find a ResolvedSignal by name in inputs or outputs
ResolvedSignal* findSignal(ResolvedModule& module, const std::string& name) {
    if (auto it = module.inputs.find(name); it != module.inputs.end()) return &it->second;
    if (auto it = module.outputs.find(name); it != module.outputs.end()) return &it->second;
    return nullptr;
}

} // anonymous namespace

void setIODomains(ResolvedModule& module, const std::string& yamlPath) {
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

            // Classify the clock input itself
            if (portClassMap.contains(info.input_port)) {
                throw CompilerError(std::format(
                    "io_domains_set: module '{}': port '{}' classified in multiple domains",
                    module.name, info.input_port));
            }
            portClassMap[info.input_port] = {PortClass::Clock, domainName, ""};

            // Expand inputs_outputs entries
            auto ioList = domainNode["inputs_outputs"];
            if (ioList && ioList.IsSequence()) {
                for (const auto& entry : ioList) {
                    auto ref = parseSignalRef(entry);
                    auto matches = expandPattern(ref.name, allPortNames);
                    if (matches.empty()) {
                        throw CompilerError(std::format(
                            "io_domains_set: wildcard '{}' in clock domain '{}' "
                            "matches no ports in module '{}'",
                            ref.name, domainName, module.name));
                    }
                    for (const auto& name : matches) {
                        if (portClassMap.contains(name)) {
                            throw CompilerError(std::format(
                                "io_domains_set: module '{}': port '{}' classified in multiple domains",
                                module.name, name));
                        }
                        portClassMap[name] = {PortClass::Sync, domainName, ref.synchronized_into};
                        info.matched_ports.push_back(name);
                        if (!ref.synchronized_into.empty()) {
                            module.synchronizedSignals[name] = ref.synchronized_into;
                        }
                    }
                }
            }

            clockDomains[domainName] = std::move(info);
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

            if (portClassMap.contains(info.signal_name)) {
                throw CompilerError(std::format(
                    "io_domains_set: module '{}': port '{}' classified in multiple domains",
                    module.name, info.signal_name));
            }
            portClassMap[info.signal_name] = {PortClass::Reset, "", ""};

            resets[resetName] = std::move(info);
        }
    }

    // Process async_domain
    if (asyncNode && asyncNode.IsSequence()) {
        for (const auto& entry : asyncNode) {
            auto ref = parseSignalRef(entry);
            auto matches = expandPattern(ref.name, allPortNames);
            if (matches.empty()) {
                throw CompilerError(std::format(
                    "io_domains_set: wildcard '{}' in async_domain "
                    "matches no ports in module '{}'",
                    ref.name, module.name));
            }
            for (const auto& name : matches) {
                if (portClassMap.contains(name)) {
                    throw CompilerError(std::format(
                        "io_domains_set: module '{}': port '{}' classified in multiple domains",
                        module.name, name));
                }
                portClassMap[name] = {PortClass::Async, "", ref.synchronized_into};
                if (!ref.synchronized_into.empty()) {
                    module.synchronizedSignals[name] = ref.synchronized_into;
                }
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

    // 3f (partial). Set sync_kind, clock_domain, clock_edge on inputs and outputs.

    // Set sync_kind on all classified IO ports
    for (const auto& [portName, cls] : portClassMap) {
        ResolvedSignal* sig = findSignal(module, portName);
        if (!sig) continue;
        switch (cls.cls) {
            case PortClass::Clock: sig->sync_kind = SyncKind::Clock; break;
            case PortClass::Reset: sig->sync_kind = SyncKind::Reset; break;
            case PortClass::Async: sig->sync_kind = SyncKind::Async; break;
            case PortClass::Sync:  sig->sync_kind = SyncKind::Sync;  break;
        }
    }

    // Set clock_domain and clock_edge on sync IO ports; also store each clock's own
    // expected edge on the clock input signal so domains_propagate_and_check can
    // validate polarity without re-parsing the YAML.
    for (const auto& [domainName, info] : clockDomains) {
        ResolvedSignal* clockSig = findSignal(module, info.input_port);
        if (!clockSig) continue;

        edge_t edge = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;

        // Store the clock's own polarity on the clock signal itself
        clockSig->clock_edge = edge;

        for (const auto& portName : info.matched_ports) {
            ResolvedSignal* sig = findSignal(module, portName);
            if (sig) {
                sig->clock_domain = clockSig;
                sig->clock_edge = edge;
            }
        }
    }

    // Store each reset's expected polarity on the reset signal itself (POSEDGE = positive,
    // NEGEDGE = negative) so domains_propagate_and_check can validate without re-parsing.
    for (const auto& [resetName, info] : resets) {
        ResolvedSignal* sig = findSignal(module, info.signal_name);
        if (sig) {
            sig->clock_edge = (info.polarity == "positive") ? POSEDGE : NEGEDGE;
        }
    }
}

} // namespace custom_hdl
