#include "frontends/systemverilog/passes/cdc_annotations.h"

#include "yaml-cpp/yaml.h"

#include <format>
#include <ranges>
#include <set>

namespace mate {

namespace {

InstancePath childPath(InstancePath path, const std::string& instanceName) {
    path.elems.push_back(instanceName);
    return path;
}

std::string pathString(const InstancePath& path) {
    std::string out;
    for (const auto& elem : path.elems) {
        if (!out.empty()) out += ".";
        out += elem;
    }
    return out.empty() ? "<top>" : out;
}

void validateNoSidecarForUnknownModule(
        const Module& module,
        const std::map<std::string, std::string>& cdcPathsByModule,
        std::set<std::string>& seenModules) {
    seenModules.insert(module.name);
    for (const auto& sub : module.hierarchyInstantiation)
        validateNoSidecarForUnknownModule(sub, cdcPathsByModule, seenModules);
}

ModuleCdcFacts parseCdcSidecar(const Module& module,
                               const InstancePath& path,
                               const std::string& yamlPath) {
    YAML::Node config = YAML::LoadFile(yamlPath);
    if (!config.IsMap()) {
        throw CompilerError(std::format(
            "cdc_annotations: '{}' must contain a YAML map", yamlPath));
    }

    for (auto it = config.begin(); it != config.end(); ++it) {
        std::string key = it->first.as<std::string>();
        if (key != "synchronizer_flops") {
            throw CompilerError(std::format(
                "cdc_annotations: '{}': unsupported key '{}'", yamlPath, key));
        }
    }

    ModuleCdcFacts facts;
    auto syncFlops = config["synchronizer_flops"];
    if (!syncFlops) return facts;
    if (!syncFlops.IsSequence()) {
        throw CompilerError(std::format(
            "cdc_annotations: '{}': synchronizer_flops must be a sequence", yamlPath));
    }

    std::set<std::string> knownFlops;
    for (const auto& flop : module.flops)
        knownFlops.insert(flop.name);

    for (const auto& entry : syncFlops) {
        if (!entry.IsScalar()) {
            throw CompilerError(std::format(
                "cdc_annotations: '{}': synchronizer_flops entries must be strings",
                yamlPath));
        }
        std::string flopName = entry.as<std::string>();
        if (!facts.synchronizer_flops.insert(flopName).second) {
            throw CompilerError(std::format(
                "cdc_annotations: '{}': duplicate synchronizer flop '{}' in module '{}' at {}",
                yamlPath, flopName, module.name, pathString(path)));
        }
        if (!knownFlops.contains(flopName)) {
            throw CompilerError(std::format(
                "cdc_annotations: '{}': unknown synchronizer flop '{}' in module '{}' at {}",
                yamlPath, flopName, module.name, pathString(path)));
        }
    }

    return facts;
}

void loadForModule(Module& module,
                   const InstancePath& path,
                   const std::map<std::string, std::string>& cdcPathsByModule,
                   FrontendDomainFacts& domainFacts) {
    ModuleDomainFacts& moduleFacts = domainFacts.getOrCreate({path, module.name});
    moduleFacts.cdc = {};

    if (auto it = cdcPathsByModule.find(module.name); it != cdcPathsByModule.end()) {
        moduleFacts.cdc = parseCdcSidecar(module, path, it->second);
    }

    for (auto& sub : module.hierarchyInstantiation)
        loadForModule(sub, childPath(path, sub.instance_name), cdcPathsByModule, domainFacts);
}

} // namespace

void loadCdcAnnotations(Module& top,
                        const std::map<std::string, std::string>& cdcPathsByModule,
                        FrontendDomainFacts& domainFacts,
                        InstancePath instancePath) {
    std::set<std::string> seenModules;
    validateNoSidecarForUnknownModule(top, cdcPathsByModule, seenModules);
    for (const auto& [moduleName, path] : cdcPathsByModule) {
        if (!seenModules.contains(moduleName)) {
            throw CompilerError(std::format(
                "cdc_annotations: sidecar '{}' names module '{}' which is not in the design",
                path, moduleName));
        }
    }
    loadForModule(top, instancePath, cdcPathsByModule, domainFacts);
}

} // namespace mate
