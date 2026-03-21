#include "passes/io_domains_set.h"

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

// Build forward adjacency: for each node, which (node, arrival_port) pairs use it?
struct FwdEdge {
    const DFGNode* user;
    // Which input slot of user this edge corresponds to
    int input_slot;
};

std::map<const DFGNode*, std::vector<FwdEdge>> buildForwardMap(const DFG& dfg) {
    std::map<const DFGNode*, std::vector<FwdEdge>> fwd;
    for (const auto& node : dfg.nodes) {
        for (int i = 0; i < static_cast<int>(node->in.size()); i++) {
            fwd[node->in[i].node].push_back({node.get(), i});
        }
    }
    return fwd;
}

// Get the flop base name from a .d signal name
std::string flopBaseName(const std::string& dName) {
    return dName.substr(0, dName.size() - 2);
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
    std::vector<std::string> asyncPorts;
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
                asyncPorts.push_back(name);
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

    // 3e. Cross-check against flop_resolve results
    // Clock inputs must be tagged Clock
    for (const auto& [domainName, info] : clockDomains) {
        ResolvedSignal* clkSig = findSignal(module, info.input_port);
        if (clkSig && clkSig->sync_kind != SyncKind::Clock) {
            throw CompilerError(std::format(
                "io_domains_set: module '{}': clock port '{}' is not tagged as Clock type "
                "(was flop_resolve run?)",
                module.name, info.input_port));
        }
    }

    // Reset inputs must be tagged Reset
    for (const auto& [resetName, info] : resets) {
        ResolvedSignal* rstSig = findSignal(module, info.signal_name);
        if (rstSig && rstSig->sync_kind != SyncKind::Reset) {
            throw CompilerError(std::format(
                "io_domains_set: module '{}': reset port '{}' is not tagged as Reset type "
                "(was flop_resolve run?)",
                module.name, info.signal_name));
        }
    }

    // Cross-check clock polarity against flop info (skip if no flops)
    if (!module.flops.empty()) {
        for (const auto& [domainName, info] : clockDomains) {
            edge_t expected = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;
            for (const auto& flop : module.flops) {
                if (flop.clock.name == info.input_port) {
                    if (flop.clock.edge != expected) {
                        throw CompilerError(std::format(
                            "io_domains_set: module '{}': clock domain '{}' polarity '{}' "
                            "does not match flop '{}' clock edge",
                            module.name, domainName, info.polarity, flop.name));
                    }
                }
            }
        }

        // Cross-check reset polarity against flop info
        for (const auto& [resetName, info] : resets) {
            edge_t expected = (info.polarity == "positive") ? POSEDGE : NEGEDGE;
            for (const auto& flop : module.flops) {
                if (flop.reset && flop.reset->name == info.signal_name) {
                    if (flop.reset->edge != expected) {
                        throw CompilerError(std::format(
                            "io_domains_set: module '{}': reset '{}' polarity '{}' "
                            "does not match flop '{}' reset edge",
                            module.name, resetName, info.polarity, flop.name));
                    }
                }
            }
        }
    }

    // 3f. Set sync_kind on all classified IO ports
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

    // Set domains on IO signals
    for (const auto& [domainName, info] : clockDomains) {
        ResolvedSignal* clockSig = findSignal(module, info.input_port);
        if (!clockSig) continue;

        edge_t edge = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;

        for (const auto& portName : info.matched_ports) {
            ResolvedSignal* sig = findSignal(module, portName);
            if (sig) {
                sig->clock_domain = clockSig;
                sig->clock_edge = edge;
            }
        }
    }

    // Async and clock/reset ports: leave clock_domain = nullptr, clock_edge = nullopt
    // (they are already default-initialized that way)

    // Also set domains on internal signals and flop types (same as old domain_resolve)
    for (auto& [name, sig] : module.signals) {
        if (sig.sync_kind != SyncKind::Clock &&
            sig.sync_kind != SyncKind::Reset) {
            // Internal signals belong to whatever domain their driving logic is in.
            // For single-clock modules this is straightforward; for multi-clock
            // we just find the first clock domain (multi-clock support is future work).
            if (clockDomains.size() == 1) {
                auto& [domainName, info] = *clockDomains.begin();
                ResolvedSignal* clockSig = findSignal(module, info.input_port);
                edge_t edge = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;
                sig.clock_domain = clockSig;
                sig.clock_edge = edge;
            }
        }
    }
    for (auto& flop : module.flops) {
        // Find the clock domain this flop belongs to
        for (const auto& [domainName, info] : clockDomains) {
            if (flop.clock.name == info.input_port) {
                ResolvedSignal* clockSig = findSignal(module, info.input_port);
                edge_t edge = (info.polarity == "posedge") ? POSEDGE : NEGEDGE;
                flop.type.clock_domain = clockSig;
                flop.type.clock_edge = edge;
                break;
            }
        }
    }

    // 3g. Fanin validation
    if (module.dfg) {
        auto fwd = buildForwardMap(*module.dfg);

        // Build a map from flop name -> clock domain name for quick lookup
        std::map<std::string, std::string> flopClockDomain;
        for (const auto& flop : module.flops) {
            for (const auto& [domainName, info] : clockDomains) {
                if (flop.clock.name == info.input_port) {
                    flopClockDomain[flop.name] = domainName;
                    break;
                }
            }
        }

        // Forward traversal from an input node, collecting .d signals reached
        auto forwardTraversal = [&](const DFGNode* start)
                -> std::vector<std::pair<std::string, std::string>> {
            // Returns pairs of (flop_name, clock_domain_name) for each .d reached
            std::vector<std::pair<std::string, std::string>> reached;
            std::set<const DFGNode*> visited;
            std::vector<const DFGNode*> worklist = {start};

            while (!worklist.empty()) {
                const DFGNode* current = worklist.back();
                worklist.pop_back();

                if (!visited.insert(current).second) continue;

                // If this is a .d signal, record it
                if (current->op == DFGOp::OUTPUT && current->name.ends_with(".d")) {
                    std::string base = flopBaseName(current->name);
                    auto it = flopClockDomain.find(base);
                    if (it != flopClockDomain.end()) {
                        reached.push_back({base, it->second});
                    }
                    continue; // Don't traverse past .d
                }

                // Stop at .q signals (flop outputs don't propagate the input domain)
                if (current->op == DFGOp::INPUT && current->name.ends_with(".q")) {
                    continue;
                }

                // Follow forward edges
                if (auto it = fwd.find(current); it != fwd.end()) {
                    for (const auto& edge : it->second) {
                        worklist.push_back(edge.user);
                    }
                }
            }
            return reached;
        };

        // Validate sync domain inputs
        for (const auto& [portName, cls] : portClassMap) {
            if (cls.cls != PortClass::Sync) continue;

            // Only validate inputs (outputs don't feed into flops from outside)
            auto inputIt = module.inputs.find(portName);
            if (inputIt == module.inputs.end() || !inputIt->second.dfg_node) continue;

            auto reached = forwardTraversal(inputIt->second.dfg_node);
            for (const auto& [flopName, flopDomain] : reached) {
                // Same domain: always allowed
                if (flopDomain == cls.clock_name) continue;
                // Declared crossing: allowed only into the specified domain
                if (!cls.synchronized_into.empty() && flopDomain == cls.synchronized_into) continue;
                throw CompilerError(std::format(
                    "io_domains_set: module '{}': sync input '{}' (domain '{}') "
                    "feeds flop '{}' in domain '{}' — cross-domain violation",
                    module.name, portName, cls.clock_name, flopName, flopDomain));
            }
        }

        // Validate async domain inputs
        for (const auto& portName : asyncPorts) {
            // Only validate inputs
            auto inputIt2 = module.inputs.find(portName);
            if (inputIt2 == module.inputs.end() || !inputIt2->second.dfg_node) continue;

            const auto& cls = portClassMap.at(portName);

            auto reached = forwardTraversal(inputIt2->second.dfg_node);
            for (const auto& [flopName, flopDomain] : reached) {
                // Declared crossing: allowed only into the specified domain
                if (!cls.synchronized_into.empty() && flopDomain == cls.synchronized_into) continue;
                throw CompilerError(std::format(
                    "io_domains_set: module '{}': async input '{}' feeds flop '{}' in domain '{}'"
                    "{}",
                    module.name, portName, flopName, flopDomain,
                    cls.synchronized_into.empty()
                        ? " without synchronizer"
                        : std::format(" — declared synchronized_into '{}' but crosses into '{}'",
                                      cls.synchronized_into, flopDomain)));
            }
        }
    }
}

} // namespace custom_hdl
