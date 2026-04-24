#include "frontends/systemverilog/systemverilog_pipeline.h"

#include "frontends/systemverilog/passes/combo_deps.h"
#include "frontends/systemverilog/passes/concat_cleanup.h"
#include "frontends/systemverilog/passes/condition_normalization.h"
#include "frontends/systemverilog/passes/constant_fold.h"
#include "frontends/systemverilog/passes/dce.h"
#include "frontends/systemverilog/passes/dfg_inline.h"
#include "frontends/systemverilog/passes/domains_propagate_and_check.h"
#include "frontends/systemverilog/passes/elaboration.h"
#include "frontends/systemverilog/passes/flop_resolve.h"
#include "frontends/systemverilog/passes/io_domains_set.h"
#include "frontends/systemverilog/passes/type_propagation.h"
#include "util/debug.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <unordered_set>

namespace mate {

namespace {

std::map<std::string, std::string> loadDomainPathsByModule(
        const std::vector<std::string>& domainFiles) {
    std::map<std::string, std::string> domainPathsByModule;
    for (const auto& path : domainFiles) {
        YAML::Node cfg = YAML::LoadFile(path);
        if (!cfg["module_name"]) {
            throw CompilerError(std::format(
                "Domain file '{}' is missing 'module_name' key", path));
        }
        std::string modName = cfg["module_name"].as<std::string>();
        if (domainPathsByModule.contains(modName)) {
            throw CompilerError(std::format(
                "Duplicate domain file for module '{}': '{}' and '{}'",
                modName, domainPathsByModule[modName], path));
        }
        domainPathsByModule[modName] = path;
    }
    return domainPathsByModule;
}

void validateDebugSpecsBeforePipeline(const Module& topModule,
                                      const std::vector<DebugNodeSpec>& specs) {
    if (specs.empty()) return;

    std::set<size_t> foundSpecs;
    std::function<void(const Module&, const std::string&)> validate =
            [&](const Module& module, const std::string& currentPath) {
        if (!module.dfg) return;
        for (const auto& sub : module.hierarchyInstantiation)
            validate(sub, currentPath + "." + sub.name);

        for (size_t i = 0; i < specs.size(); i++) {
            const auto& spec = specs[i];
            if (!spec.module_path.empty() &&
                currentPath != spec.module_path &&
                !currentPath.ends_with("." + spec.module_path))
                continue;

            bool found = module.dfg->hasSignal("", spec.node_name) ||
                         module.dfg->hasOutput("", spec.node_name);

            if (!found && !spec.module_path.empty())
                throw CompilerError(std::format(
                    "debug_dfg_nodes: node '{}' not found in module '{}' signals or outputs",
                    spec.node_name, module.name));

            if (found) foundSpecs.insert(i);
        }
    };

    validate(topModule, topModule.name);

    for (size_t i = 0; i < specs.size(); i++) {
        if (!foundSpecs.contains(i))
            throw CompilerError(std::format(
                "debug_dfg_nodes: node '{}' not found in any module signals or outputs",
                specs[i].node_name));
    }
}

void runMateIRPipeline(MateIR& ir,
                       const std::map<std::string, std::string>& domainPathsByModule,
                       const std::vector<DebugNodeSpec>& debugSpecs) {
    Module& topModule = ir.top;

    validateDebugSpecsBeforePipeline(topModule, debugSpecs);

    std::set<size_t> satisfiedDebugSpecs;

    std::function<void(Module&, const std::string&)> runPipeline =
            [&](Module& module, const std::string& currentPath) {
        if (!module.dfg) return;

        std::cout << "========================================" << std::endl;
        std::cout << "Module: " << module.name << std::endl;
        std::cout << "========================================" << std::endl;

        auto findInlinedNode = [&](const std::string& modulePath,
                                   const std::string& nodeName) -> const DFGNode* {
            std::function<const DFGNode*(const Module&, const std::string&)> recurse =
                [&](const Module& mod, const std::string& prefix) -> const DFGNode* {
                    for (const auto& sub : mod.hierarchyInstantiation) {
                        std::string subPath = prefix.empty() ? sub.instance_name
                                                             : prefix + "." + sub.instance_name;
                        if (sub.name == modulePath) {
                            if (auto* n = module.dfg->getSignalNode(subPath, nodeName)) return n;
                            if (auto* n = module.dfg->getOutputNode(subPath, nodeName)) return n;
                        }
                        if (auto* n = recurse(sub, subPath)) return n;
                    }
                    return nullptr;
                };
            return recurse(module, "");
        };

        auto runPass = [&](const int number, const std::string& passName, auto passFn) {
            std::cout << "========================================" << std::endl;
            std::cout << "Performing " << passName << "..." << std::endl;
            std::cout << "========================================" << std::endl;
            module.dfg->validate();

            try {
                passFn();
                module.dfg->validate();
            } catch (const CompilerError& e) {
                std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
                std::filesystem::create_directories(dir);
                std::set<const DFGNode*> errorNodes;
                if (e.errorNode) errorNodes.insert(e.errorNode);
                std::ofstream(std::format("{}/{:02}_{}_ERROR.dot", dir, number, passName))
                    << module.dfg->toDot(passName + "_ERROR", errorNodes);
                throw;
            }

            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::filesystem::create_directories(dir);
            std::ofstream(std::format("{}/{:02}_{}.dot", dir, number, passName)) << module.dfg->toDot(passName);
            std::ofstream(std::format("{}/{:02}_{}.json", dir, number, passName)) << module.dfg->toJson();

            if (!debugSpecs.empty()) {
                for (size_t specIdx = 0; specIdx < debugSpecs.size(); specIdx++) {
                    const auto& spec = debugSpecs[specIdx];

                    const DFGNode* node = nullptr;
                    bool pathMatches = spec.module_path.empty() ||
                                       currentPath == spec.module_path ||
                                       currentPath.ends_with("." + spec.module_path);

                    if (pathMatches) {
                        if (auto* n = module.dfg->getSignalNode("", spec.node_name))
                            node = n;
                        else if (auto* n = module.dfg->getOutputNode("", spec.node_name))
                            node = n;

                        if (!node) {
                            if (!spec.module_path.empty())
                                throw CompilerError(std::format(
                                    "debug_dfg_nodes: node '{}' not found in module '{}' signals or outputs",
                                    spec.node_name, module.name));
                            continue;
                        }
                    } else {
                        node = findInlinedNode(spec.module_path, spec.node_name);
                        if (!node) continue;
                    }

                    satisfiedDebugSpecs.insert(specIdx);
                    std::ofstream(std::format("{}/{:02}_{}_cone_{}.dot", dir, number, passName, spec.node_name))
                        << module.dfg->toDotCone(node, passName + "_cone_" + spec.node_name);
                    std::ofstream(std::format("{}/{:02}_{}_cone_{}.json", dir, number, passName, spec.node_name))
                        << module.dfg->toJsonCone(node);
                }
            }
        };

        std::function<void(const Module&, std::ostream&)> dumpFlopsRecursive =
            [&](const Module& mod, std::ostream& f) {
                if (!mod.flops.empty()) {
                    f << "=== " << mod.name << " ===\n";
                    for (const auto& flop : mod.flops)
                        flop.print(f);
                }
                for (const auto& sub : mod.hierarchyInstantiation)
                    dumpFlopsRecursive(sub, f);
            };

        runPass(0, "elaboration", []{});
        runPass(1, "dfg_inline", [&]{ inlineDFGs(module); });
        runPass(2, "concat_cleanup", [&]{ cleanupConcats(*module.dfg); });
        runPass(3, "constant_fold", [&]{ constantFold(*module.dfg); });
        runPass(4, "type_propagation", [&]{ propagateTypes(*module.dfg); });
        runPass(5, "condition_normalization", [&]{ normalizeConditions(*module.dfg); });
        runPass(6, "constant_fold", [&]{ constantFold(*module.dfg); });
        runPass(7, "condition_normalization", [&]{ normalizeConditions(*module.dfg); });
        runPass(8, "constant_fold", [&]{ constantFold(*module.dfg); });
        runPass(9, "io_domains_set", [&]{
            std::function<void(Module&)> setDomains = [&](Module& mod) {
                auto it = domainPathsByModule.find(mod.name);
                if (it == domainPathsByModule.end()) {
                    throw CompilerError(std::format(
                        "No domains file provided for module '{}' "
                        "(use --domains to specify)", mod.name));
                }
                setIODomains(mod, it->second);
                for (auto& sub : mod.hierarchyInstantiation)
                    setDomains(sub);
            };
            setDomains(module);
        });
        runPass(10, "flop_resolve", [&]{ resolveFlops(module); });
        {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::ofstream f(std::format("{}/10_flop_resolve_flops.txt", dir));
            dumpFlopsRecursive(module, f);
        }
        runPass(11, "dce", [&]{
            std::unordered_set<DFGNode*> keepAlive;
            std::function<void(const Module&)> collect = [&](const Module& mod) {
                for (const auto& parameter : mod.parameters)
                    if (parameter.dfg_node) keepAlive.insert(parameter.dfg_node);
                for (const auto& parameter : mod.localparams)
                    if (parameter.dfg_node) keepAlive.insert(parameter.dfg_node);
                for (const auto& [name, sig] : mod.inputs)
                    for (auto* leaf : signalLeaves(sig)) if (leaf) keepAlive.insert(leaf);
                for (const auto& [name, sig] : mod.outputs)
                    for (auto* leaf : signalLeaves(sig)) if (leaf) keepAlive.insert(leaf);
                for (const auto& [name, sig] : mod.signals)
                    for (auto* leaf : signalLeaves(sig)) if (leaf) keepAlive.insert(leaf);
                for (const auto& flop : mod.flops) {
                    for (auto* leaf : flopDLeaves(flop)) if (leaf) keepAlive.insert(leaf);
                    for (auto* leaf : flopQLeaves(flop)) if (leaf) keepAlive.insert(leaf);
                }
                for (const auto& sub : mod.hierarchyInstantiation)
                    collect(sub);
            };
            collect(module);
            eliminateDeadCode(*module.dfg, keepAlive);
        });
        module.dfg->validateNoOrphans();
        module.dfg->validateStrictLiveDFG();
        runPass(12, "domains_propagate_and_check", [&]{ domainsPropagateAndCheck(module); });
        {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::ofstream f(std::format("{}/12_domains_propagate_flops.txt", dir));
            dumpFlopsRecursive(module, f);
        }
        computeComboDeps(module);
        validateNoCombLoops(module);

        if (!debugSpecs.empty()) {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            for (const auto& spec : debugSpecs) {
                const DFGNode* node = nullptr;
                bool pathMatches = spec.module_path.empty() ||
                                   currentPath == spec.module_path ||
                                   currentPath.ends_with("." + spec.module_path);
                if (pathMatches) {
                    if (auto* n = module.dfg->getSignalNode("", spec.node_name)) node = n;
                    else if (auto* n = module.dfg->getOutputNode("", spec.node_name)) node = n;
                } else {
                    node = findInlinedNode(spec.module_path, spec.node_name);
                }
                if (!node) continue;
                std::ofstream(std::format("{}/{}.dot", dir, spec.node_name))
                    << module.dfg->toDotCone(node, spec.node_name);
                std::ofstream(std::format("{}/{}.json", dir, spec.node_name))
                    << module.dfg->toJsonCone(node);
            }
        }
    };

    runPipeline(topModule, topModule.name);

    std::string dir = DEBUG_OUTPUT_DIR + "/" + topModule.name;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/hierarchy.json") << topModule.toJson();
}

} // namespace

MateIR lowerSystemVerilogToMateIR(ExtractedIR& extracted,
                                  const slang::SourceManager& sourceManager,
                                  const SystemVerilogCompileOptions& options) {
    MateIR ir;
    ir.top = [&]() -> Module {
        if (options.top_module) {
            ParameterContext topParams;
            for (const auto& [name, value] : options.parameters) {
                topParams.values[name] = static_cast<int>(value);
            }
            return resolveModules(extracted.modules, extracted.packages, extracted.globalImports,
                                  sourceManager, *options.top_module, topParams);
        }
        return resolveModules(extracted.modules, extracted.packages, extracted.globalImports,
                              sourceManager);
    }();
    ir.source_files = options.source_files;
    ir.frontend_module_count = extracted.modules.size();

    auto domainPathsByModule = loadDomainPathsByModule(options.domain_files);
    runMateIRPipeline(ir, domainPathsByModule, options.debug_dfg_nodes);

    return ir;
}

} // namespace mate
