#include "frontends/systemverilog/systemverilog_pipeline.h"

#include "frontends/systemverilog/passes/cdc_annotations.h"
#include "frontends/systemverilog/passes/cdc_emit.h"
#include "frontends/systemverilog/passes/condition_normalization.h"
#include "frontends/systemverilog/passes/constant_fold.h"
#include "frontends/systemverilog/passes/dce.h"
#include "frontends/systemverilog/passes/dfg_inline.h"
#include "frontends/systemverilog/passes/domains_propagate_and_check.h"
#include "frontends/systemverilog/passes/elaboration.h"
#include "frontends/systemverilog/passes/flop_resolve.h"
#include "frontends/systemverilog/passes/global_domain_resolve.h"
#include "frontends/systemverilog/passes/io_domains_set.h"
#include "frontends/systemverilog/passes/top_io_domains_emit.h"
#include "frontends/systemverilog/passes/top_io_domains_infer.h"
#include "frontends/systemverilog/passes/type_propagation.h"
#include "frontends/systemverilog/domain_facts.h"
#include "mateir/lang_metadata.h"
#include "util/debug.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <set>
#include <unordered_set>

namespace mate {

namespace {

std::string loadTopDomainPath(
        const std::vector<std::string>& domainFiles,
        const std::string& topModuleName) {
    if (domainFiles.empty()) {
        throw CompilerError(std::format(
            "No top-level domains file provided for module '{}' "
            "(use --domains or --infer-top-domains)", topModuleName));
    }
    std::optional<std::string> topDomainPath;
    for (const auto& path : domainFiles) {
        YAML::Node cfg = YAML::LoadFile(path);
        if (!cfg["module_name"]) {
            throw CompilerError(std::format(
                "Domain file '{}' is missing 'module_name' key", path));
        }
        std::string modName = cfg["module_name"].as<std::string>();
        if (modName != topModuleName) {
            throw CompilerError(std::format(
                "Domain file '{}' is for non-top module '{}'; main domains YAML "
                "is only supported for top module '{}'",
                path, modName, topModuleName));
        }
        if (topDomainPath) {
            throw CompilerError(std::format(
                "Duplicate top-level domain file for module '{}': '{}' and '{}'",
                topModuleName, *topDomainPath, path));
        }
        topDomainPath = path;
    }
    return *topDomainPath;
}

std::map<std::string, std::string> loadCdcPathsByModule(
        const std::vector<std::string>& sourceFiles,
        const std::vector<std::string>& domainFiles) {
    std::set<std::filesystem::path> searchDirs;
    for (const auto& path : sourceFiles)
        searchDirs.insert(std::filesystem::path(path).parent_path());
    for (const auto& path : domainFiles)
        searchDirs.insert(std::filesystem::path(path).parent_path());

    std::map<std::string, std::string> cdcPathsByModule;
    for (const auto& dir : searchDirs) {
        if (dir.empty() || !std::filesystem::exists(dir)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string filename = entry.path().filename().string();
            std::string moduleName;
            if (filename.ends_with(".cdc.yaml")) {
                moduleName = filename.substr(0, filename.size() - std::string(".cdc.yaml").size());
            } else if (filename.ends_with(".cdc.yml")) {
                moduleName = filename.substr(0, filename.size() - std::string(".cdc.yml").size());
            } else {
                continue;
            }

            std::string path = entry.path().string();
            if (cdcPathsByModule.contains(moduleName)) {
                throw CompilerError(std::format(
                    "Duplicate CDC sidecar for module '{}': '{}' and '{}'",
                    moduleName, cdcPathsByModule[moduleName], path));
            }
            cdcPathsByModule[moduleName] = path;
        }
    }
    return cdcPathsByModule;
}

bool debugPathMatches(const std::string& currentPath,
                      const std::string& modulePath) {
    return modulePath.empty() ||
           currentPath == modulePath ||
           currentPath.ends_with("." + modulePath);
}

const DFGNode* findDebugLeaf(const Module& module,
                             const std::string& nodeName) {
    return findModuleDebugLeafNode(module, nodeName);
}

void validateDebugSpecsBeforePipeline(const Module& topModule,
                                      const std::vector<DebugNodeSpec>& specs,
                                      const std::string& optionName) {
    if (specs.empty()) return;

    std::set<size_t> foundSpecs;
    std::function<void(const Module&, const std::string&)> validate =
            [&](const Module& module, const std::string& currentPath) {
        for (const auto& sub : module.hierarchyInstantiation)
            validate(sub, currentPath + "." + sub.instance_name);

        for (size_t i = 0; i < specs.size(); i++) {
            const auto& spec = specs[i];
            if (!debugPathMatches(currentPath, spec.module_path))
                continue;

            bool found = findDebugLeaf(module, spec.node_name) != nullptr;

            if (!found && !spec.module_path.empty())
                throw CompilerError(std::format(
                    "{}: node '{}' not found in module '{}' outputs, signals, or flop .d/.q leaves",
                    optionName, spec.node_name, module.name));

            if (found) foundSpecs.insert(i);
        }
    };

    validate(topModule, topModule.name);

    for (size_t i = 0; i < specs.size(); i++) {
        if (!foundSpecs.contains(i))
            throw CompilerError(std::format(
                "{}: node '{}' not found in any module outputs, signals, or flop .d/.q leaves",
                optionName, specs[i].node_name));
    }
}

void runMateIRPipeline(MateIR& ir,
                       LangMetadata&& langMeta,
                       const std::optional<std::string>& topDomainPath,
                       TopDomainMode topDomainMode,
                       bool inferSynchronizers,
                       const std::optional<std::string>& emitInferredDomainsPath,
                       const std::optional<std::string>& emitInferredSynchronizersDir,
                       const std::map<std::string, std::string>& cdcPathsByModule,
                       FrontendDomainFacts& domainFacts,
                       const std::vector<DebugNodeSpec>& debugSpecs,
                       bool dumpPasses) {
    Module& topModule = ir.top;

    validateDebugSpecsBeforePipeline(topModule, debugSpecs, "debug_dfg_nodes");

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
                    if (debugPathMatches(prefix, modulePath)) {
                        if (auto* n = findDebugLeaf(mod, nodeName)) return n;
                    }
                    for (const auto& sub : mod.hierarchyInstantiation) {
                        const std::string subPath = prefix.empty()
                            ? sub.instance_name
                            : prefix + "." + sub.instance_name;
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
            if (dumpPasses || !debugSpecs.empty()) {
                std::filesystem::create_directories(dir);
            }
            if (dumpPasses) {
                std::ofstream(std::format("{}/{:02}_{}.dot", dir, number, passName)) << module.dfg->toDot(passName);
                std::ofstream(std::format("{}/{:02}_{}.json", dir, number, passName)) << module.dfg->toJson();
            }

            if (!debugSpecs.empty()) {
                for (size_t specIdx = 0; specIdx < debugSpecs.size(); specIdx++) {
                    const auto& spec = debugSpecs[specIdx];

                    const DFGNode* node = nullptr;
                    bool pathMatches = debugPathMatches(currentPath, spec.module_path);

                    if (pathMatches) {
                        node = findDebugLeaf(module, spec.node_name);

                        if (!node) {
                            if (!spec.module_path.empty())
                                throw CompilerError(std::format(
                                    "debug_dfg_nodes: node '{}' not found in module '{}' outputs, signals, or flop .d/.q leaves",
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
        auto collectRoots = [&](const ModuleRootSelection& selection) {
            std::unordered_set<DFGNode*> roots;
            collectModuleRoots(module, roots, selection);
            return roots;
        };
        const auto preOptRoots = collectRoots(ModuleRootSelection{
            .internal_nodes = true,
            .recurse_hierarchy = true,
        });
        runPass(2, "constant_fold", [&]{ constantFold(*module.dfg, preOptRoots); });
        runPass(3, "type_propagation", [&]{ propagateTypes(*module.dfg); });
        runPass(4, "condition_normalization", [&]{ normalizeConditions(*module.dfg, preOptRoots); });
        runPass(5, "constant_fold", [&]{ constantFold(*module.dfg, preOptRoots); });
        runPass(6, "flop_resolve", [&]{ resolveFlops(module, domainFacts); });
        if (dumpPasses) {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::ofstream f(std::format("{}/06_flop_resolve_flops.txt", dir));
            dumpFlopsRecursive(module, f);
        }
        if (topDomainMode == TopDomainMode::Yaml) {
            runPass(7, "load_top_io_domains", [&]{
                loadTopIODomains(module, *topDomainPath, domainFacts);
            });
        } else {
            runPass(7, "infer_top_clock_reset_domains", [&]{
                inferTopClockResetDomains(module, domainFacts);
            });
        }
        runPass(8, "cdc_annotations", [&]{
            if (inferSynchronizers) {
                loadCdcAnnotations(module, {}, domainFacts);
            } else {
                loadCdcAnnotations(module, cdcPathsByModule, domainFacts);
            }
        });
        runPass(9, "global_domain_resolve", [&]{
            resolveGlobalDomains(ir, domainFacts);
        });
        if (topDomainMode == TopDomainMode::Infer) {
            runPass(10, "infer_top_data_input_domains", [&]{
                inferTopDataInputDomains(ir, domainFacts);
            });
        }
        runPass(topDomainMode == TopDomainMode::Infer ? 11 : 10, "dce", [&]{
            auto keepAlive = collectRoots(ModuleRootSelection{
                .parameters = true,
                .localparams = true,
                .inputs = true,
                .outputs = true,
                .internal_nodes = true,
                .flop_d = true,
                .flop_q = true,
                .recurse_hierarchy = true,
            });
            eliminateDeadCode(*module.dfg, keepAlive);
        });
        module.dfg->validateNoOrphans();
        module.dfg->validateStrictLiveDFG();
        validateFrontendDomainFacts(module, domainFacts);
        runPass(topDomainMode == TopDomainMode::Infer ? 12 : 11,
                "domains_propagate_and_check",
                [&]{ domainsPropagateAndCheck(ir, domainFacts, inferSynchronizers); });
        if (dumpPasses) {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::ofstream f(std::format("{}/{}_domains_propagate_flops.txt",
                                        dir,
                                        topDomainMode == TopDomainMode::Infer ? "12" : "11"));
            dumpFlopsRecursive(module, f);
        }
        validateNoCombLoops(module);

        if (!debugSpecs.empty()) {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::filesystem::create_directories(dir);
            for (const auto& spec : debugSpecs) {
                const DFGNode* node = nullptr;
                bool pathMatches = debugPathMatches(currentPath, spec.module_path);
                if (pathMatches) {
                    node = findDebugLeaf(module, spec.node_name);
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

    // Sidecar integrity rests on this invariant: named Module entities are
    // created by elaboration and never renamed nor deleted by any pass.
    const NamedEntitySnapshot namedEntitiesBefore = collectNamedEntities(topModule);

    runPipeline(topModule, topModule.name);

    assertNamedEntitiesUnchanged(namedEntitiesBefore, topModule);

    // Attach the language-metadata sidecar only now that all passes have run:
    // passes never had an access path to it. Then verify every record.
    ir.lang_metadata = std::move(langMeta);
    validateLangMetadata(ir.lang_metadata, ir.top);

    std::string dir = DEBUG_OUTPUT_DIR + "/" + topModule.name;
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/hierarchy.json") << hierarchyToJson(ir);
    std::ofstream(dir + "/lang_metadata.json")
        << langMetadataToJson(ir.lang_metadata) << "\n";

    if (topDomainMode == TopDomainMode::Infer && emitInferredDomainsPath) {
        emitInferredTopDomainsYaml(topModule, domainFacts, *emitInferredDomainsPath);
    }
    if (inferSynchronizers && emitInferredSynchronizersDir) {
        emitInferredCdcYamls(domainFacts, *emitInferredSynchronizersDir);
    }
}

} // namespace

MateIR lowerSystemVerilogToMateIR(ExtractedIR& extracted,
                                  const slang::SourceManager& sourceManager,
                                  const SystemVerilogCompileOptions& options) {
    MateIR ir;
    FrontendDomainFacts domainFacts;
    LangMetadata langMeta;
    ir.top = [&]() -> Module {
        if (options.top_module) {
            ParameterContext topParams;
            for (const auto& [name, value] : options.parameters) {
                topParams.values[name] =
                    ConstantValue::bits(Type::makeInteger(64, true), value);
            }
            return resolveModules(extracted.modules, extracted.packages, extracted.interfaces,
                                  extracted.globalImports,
                                  sourceManager, *options.top_module, topParams, &domainFacts,
                                  &langMeta);
        }
        return resolveModules(extracted.modules, extracted.packages, extracted.interfaces,
                              extracted.globalImports,
                              sourceManager, &domainFacts, &langMeta);
    }();
    ir.source_files = options.source_files;
    ir.frontend_module_count = extracted.modules.size();

    // sv.port_type records: the externally-referenceable qualified type name
    // of a top-level port (e.g. "pkg::payload_t") is a source fact destroyed
    // by lowering and not recomputable from the IR.
    {
        const UnresolvedModule* topUnresolved = nullptr;
        for (const auto& module : extracted.modules) {
            if (module->name == ir.top.name) {
                topUnresolved = module.get();
                break;
            }
        }
        if (!topUnresolved) {
            throw CompilerError(std::format(
                "top module '{}' not found in extracted modules", ir.top.name));
        }
        auto emitPortTypes = [&](const std::vector<UnresolvedSignal>& signals) {
            for (const auto& signal : signals) {
                auto qualified = externalPortTypeName(
                    signal, extracted.globalImports, topUnresolved->headerImports,
                    extracted.packages);
                if (!qualified) continue;
                LangMetaRecord record;
                record.kind = "sv.port_type";
                record.name = signal.name;
                record.attrs["type"] = *qualified;
                record.bindings.push_back(
                    {"port", {"", "module_node", signal.name}});
                langMeta.records.push_back(std::move(record));
            }
        };
        emitPortTypes(topUnresolved->inputs);
        emitPortTypes(topUnresolved->outputs);
    }

    std::optional<std::string> topDomainPath;
    if (options.top_domain_mode == TopDomainMode::Yaml) {
        topDomainPath = loadTopDomainPath(options.domain_files, ir.top.name);
    }
    auto cdcPathsByModule = loadCdcPathsByModule(options.source_files, options.domain_files);
    runMateIRPipeline(ir, std::move(langMeta), topDomainPath, options.top_domain_mode,
                      options.infer_synchronizers,
                      options.emit_inferred_domains_path,
                      options.emit_inferred_synchronizers_dir,
                      cdcPathsByModule, domainFacts, options.debug_dfg_nodes,
                      options.dump_passes);

    return ir;
}

} // namespace mate
