#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/types.h>

#include "passes/extractor.h"
#include "passes/elaboration.h"
#include "passes/concat_cleanup.h"
#include "passes/constant_fold.h"
#include "passes/condition_normalization.h"
#include "passes/dce.h"
#include "passes/dfg_inline.h"
#include "passes/flop_resolve.h"
#include "passes/combo_deps.h"
#include "passes/domains_propagate_and_check.h"
#include "passes/io_domains_set.h"
#include "passes/type_propagation.h"
#include "sim/simulator.h"
#include "util/debug.h"
#include "util/source_loc.h"

#include "yaml-cpp/yaml.h"
#include "slang/syntax/SyntaxTree.h"

using namespace slang::syntax;
using namespace custom_hdl;

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [--domains <f1.yaml> [f2.yaml ...]] <verilog_file>" << std::endl;
    std::cerr << "       " << progName << " --simulate --top <module>"
              << " --inputs-dir <dir> --output-dir <dir>" << std::endl;
    std::cerr << "           [--flops-initial <random|zeros|ones>] [--flops-seed <n>]" << std::endl;
    std::cerr << "           [--debug-nodes <n1,n2,...>] [--params <K=V,K=V,...>]" << std::endl;
    std::cerr << "           [--domains <f1.yaml> [f2.yaml ...]]" << std::endl;
    std::cerr << "           <source1.v> [source2.v ...]" << std::endl;
    std::cerr << "\nOptions:" << std::endl;
    std::cerr << "  --simulate                Run cycle-based simulation" << std::endl;
    std::cerr << "  --top <module>            Top module name (simulate mode)" << std::endl;
    std::cerr << "  --inputs-dir <dir>        Directory containing input stimuli files" << std::endl;
    std::cerr << "  --output-dir <dir>        Directory for simulation output" << std::endl;
    std::cerr << "  --flops-initial <mode>    Flop init: random (default), zeros, ones" << std::endl;
    std::cerr << "  --flops-seed <n>          Seed for random flop initialization" << std::endl;
    std::cerr << "  --debug-nodes <n1,n2,...> Comma-separated node names for debug DOTs" << std::endl;
    std::cerr << "  --params <K=V,K=V,...>    Comma-separated parameter overrides" << std::endl;
    std::cerr << "  --domains <f1> [f2 ...]   Domain YAML files (one per module)" << std::endl;
}

// Split a comma-separated string into a vector of strings
static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

int main(int argc, char** argv) {
    // Parse command line arguments
    bool simulateMode = false;
    std::string topModule;
    std::string inputsDir;
    std::string outputDir;
    std::string flopsInitialStr;
    std::string flopsSeedStr;
    std::string debugNodesStr;
    std::string paramsStr;
    std::vector<std::string> domainFiles;
    std::vector<std::string> sourceFiles;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--simulate") == 0) {
            simulateMode = true;
        } else if (std::strcmp(argv[i], "--top") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --top requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            topModule = argv[++i];
        } else if (std::strcmp(argv[i], "--inputs-dir") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --inputs-dir requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            inputsDir = argv[++i];
        } else if (std::strcmp(argv[i], "--output-dir") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --output-dir requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            outputDir = argv[++i];
        } else if (std::strcmp(argv[i], "--flops-initial") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --flops-initial requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            flopsInitialStr = argv[++i];
        } else if (std::strcmp(argv[i], "--flops-seed") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --flops-seed requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            flopsSeedStr = argv[++i];
        } else if (std::strcmp(argv[i], "--debug-nodes") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --debug-nodes requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            debugNodesStr = argv[++i];
        } else if (std::strcmp(argv[i], "--params") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --params requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            paramsStr = argv[++i];
        } else if (std::strcmp(argv[i], "--domains") == 0) {
            // Collect following .yaml/.yml arguments until a non-YAML arg
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string_view arg(argv[i + 1]);
                if (!arg.ends_with(".yaml") && !arg.ends_with(".yml")) break;
                domainFiles.push_back(argv[++i]);
            }
            if (domainFiles.empty()) {
                std::cerr << "ERROR: --domains requires at least one .yaml file" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            std::cerr << "ERROR: Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        } else {
            sourceFiles.push_back(argv[i]);
        }
    }

    // Build SimConfig if in simulate mode
    std::optional<SimConfig> simConfig;
    std::string filename;  // used in non-simulate mode

    if (simulateMode) {
        if (topModule.empty()) {
            std::cerr << "ERROR: --simulate requires --top <module>" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        if (inputsDir.empty()) {
            std::cerr << "ERROR: --simulate requires --inputs-dir <dir>" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        if (outputDir.empty()) {
            std::cerr << "ERROR: --simulate requires --output-dir <dir>" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        if (sourceFiles.empty()) {
            std::cerr << "ERROR: --simulate requires at least one source file" << std::endl;
            printUsage(argv[0]);
            return 1;
        }

        SimConfig cfg;
        cfg.source_files = sourceFiles;
        cfg.top_module = topModule;
        cfg.inputs_dir = inputsDir;
        cfg.output_dir = outputDir;

        if (!flopsInitialStr.empty()) {
            if (flopsInitialStr == "random") {
                cfg.flops_initial = FlopsInitial::Random;
            } else if (flopsInitialStr == "zeros") {
                cfg.flops_initial = FlopsInitial::AllZeros;
            } else if (flopsInitialStr == "ones") {
                cfg.flops_initial = FlopsInitial::AllOnes;
            } else {
                std::cerr << "ERROR: --flops-initial must be random, zeros, or ones" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
        }

        if (!flopsSeedStr.empty()) {
            cfg.flops_initial_seed = std::stoull(flopsSeedStr);
        }

        if (!debugNodesStr.empty()) {
            for (const auto& entry : splitComma(debugNodesStr)) {
                auto colon = entry.find(':');
                if (colon == std::string::npos)
                    cfg.debug_dfg_nodes.push_back({"", entry});
                else
                    cfg.debug_dfg_nodes.push_back({entry.substr(0, colon), entry.substr(colon + 1)});
            }
        }

        if (!paramsStr.empty()) {
            for (const auto& kv : splitComma(paramsStr)) {
                auto eq = kv.find('=');
                if (eq == std::string::npos) {
                    std::cerr << "ERROR: --params entry '" << kv << "' must be KEY=VALUE" << std::endl;
                    printUsage(argv[0]);
                    return 1;
                }
                cfg.parameters[kv.substr(0, eq)] = std::stoll(kv.substr(eq + 1));
            }
        }

        simConfig = std::move(cfg);
    } else {
        // Non-simulate mode: single source file
        if (sourceFiles.size() != 1) {
            if (sourceFiles.empty()) {
                printUsage(argv[0]);
            } else {
                std::cerr << "ERROR: Multiple input files only supported in --simulate mode" << std::endl;
                printUsage(argv[0]);
            }
            return 1;
        }
        filename = sourceFiles[0];
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Custom HDL Compiler" << std::endl;
    std::cout << "========================================" << std::endl;
    if (simulateMode) {
        std::cout << "Parsing: ";
        for (size_t i = 0; i < simConfig->source_files.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << simConfig->source_files[i];
        }
        std::cout << std::endl;
    } else {
        std::cout << "Parsing: " << filename << std::endl;
    }
    std::cout << "----------------------------------------" << std::endl;

    // Parse the Verilog file(s) using Slang
    std::shared_ptr<SyntaxTree> tree;
    if (simulateMode) {
        std::vector<std::string_view> paths(simConfig->source_files.begin(),
                                            simConfig->source_files.end());
        auto treeResult = SyntaxTree::fromFiles(std::span<const std::string_view>(paths));
        if (!treeResult) {
            std::cerr << "ERROR: Failed to load source files" << std::endl;
            return 1;
        }
        tree = std::move(treeResult).value();
    } else {
        auto treeResult = SyntaxTree::fromFile(filename);
        if (!treeResult) {
            std::cerr << "ERROR: Failed to load file: " << filename << std::endl;
            return 1;
        }
        tree = std::move(treeResult).value();
    }

    // Check for syntax errors
    auto& diagnostics = tree->diagnostics();
    bool hasErrors = false;
    for (const auto& diag : diagnostics) {
        if (diag.isError()) {
            hasErrors = true;
            break;
        }
    }
    if (hasErrors) {
        std::cerr << "Syntax errors found: " << diagnostics.size() << " diagnostic(s)" << std::endl;
        return 1;
    }

    std::cout << "Parsing successful!" << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    try {

    auto ir = buildIR(*tree);

    // Build parameter context for top module if sim config provides parameters
    auto topModule = [&]() {
        if (simConfig && !simConfig->parameters.empty()) {
            ParameterContext topParams;
            for (const auto& [name, value] : simConfig->parameters) {
                topParams.values[name] = static_cast<int>(value);
            }
            return resolveModules(ir.modules, ir.packages, ir.globalImports,
                                  tree->sourceManager(),
                                  simConfig->top_module, topParams);
        }
        if (simConfig) {
            ParameterContext emptyCtx;
            return resolveModules(ir.modules, ir.packages, ir.globalImports,
                                  tree->sourceManager(),
                                  simConfig->top_module, emptyCtx);
        }
        return resolveModules(ir.modules, ir.packages, ir.globalImports,
                              tree->sourceManager());
    }();

    // Build module_name -> YAML path map from --domains files
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

    // Pre-flight: validate debug node specs against the initial DFGs (before any passes run).
    // This catches wrong node names regardless of whether the pipeline later fails.
    if (simConfig && !simConfig->debug_dfg_nodes.empty()) {
        std::set<size_t> foundSpecs;

        std::function<void(const ResolvedModule&, const std::string&)> validateDebugSpecs =
                [&](const ResolvedModule& module, const std::string& currentPath) {
            if (!module.dfg) return;
            for (const auto& sub : module.hierarchyInstantiation)
                validateDebugSpecs(sub, currentPath + "." + sub.name);

            for (size_t i = 0; i < simConfig->debug_dfg_nodes.size(); i++) {
                const auto& spec = simConfig->debug_dfg_nodes[i];
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
        validateDebugSpecs(topModule, topModule.name);

        for (size_t i = 0; i < simConfig->debug_dfg_nodes.size(); i++) {
            if (!foundSpecs.contains(i))
                throw CompilerError(std::format(
                    "debug_dfg_nodes: node '{}' not found in any module signals or outputs",
                    simConfig->debug_dfg_nodes[i].node_name));
        }
    }

    // Tracks which debug_dfg_nodes specs were satisfied (by index) across all modules/passes
    std::set<size_t> satisfiedDebugSpecs;

    // Run the full pass pipeline on a module.
    // After elaboration, inlines all submodule DFGs, then runs all passes once
    // on the flat top-level DFG.
    std::function<void(ResolvedModule&, const std::string&)> runPipeline =
            [&](ResolvedModule& module, const std::string& currentPath) {
        if (!module.dfg) return;

        std::cout << "========================================" << std::endl;
        std::cout << "Module: " << module.name << std::endl;
        std::cout << "========================================" << std::endl;

        // Find a debug node in the flat DFG by locating the instance whose module type
        // name matches modulePath, then looking up the instance-path-qualified node name.
        auto findInlinedNode = [&](const std::string& modulePath, const std::string& nodeName) -> const DFGNode* {
            std::function<const DFGNode*(const ResolvedModule&, const std::string&)> recurse =
                [&](const ResolvedModule& mod, const std::string& prefix) -> const DFGNode* {
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

            if (simConfig && !simConfig->debug_dfg_nodes.empty()) {
                for (size_t specIdx = 0; specIdx < simConfig->debug_dfg_nodes.size(); specIdx++) {
                    const auto& spec = simConfig->debug_dfg_nodes[specIdx];

                    const DFGNode* node = nullptr;
                    bool pathMatches = spec.module_path.empty() ||
                                       currentPath == spec.module_path ||
                                       currentPath.ends_with("." + spec.module_path);

                    if (pathMatches) {
                        // Direct lookup in this module's DFG
                        if (auto* n = module.dfg->getSignalNode("", spec.node_name))
                            node = n;
                        else if (auto* n = module.dfg->getOutputNode("", spec.node_name))
                            node = n;

                        if (!node) {
                            if (!spec.module_path.empty())
                                throw CompilerError(std::format(
                                    "debug_dfg_nodes: node '{}' not found in module '{}' signals or outputs",
                                    spec.node_name, module.name));
                            continue; // no module_path — will be validated after pipeline
                        }
                    } else {
                        node = findInlinedNode(spec.module_path, spec.node_name);
                        if (!node) continue; // not yet inlined (e.g. pre-inline passes)
                    }

                    satisfiedDebugSpecs.insert(specIdx);
                    std::ofstream(std::format("{}/{:02}_{}_cone_{}.dot", dir, number, passName, spec.node_name))
                        << module.dfg->toDotCone(node, passName + "_cone_" + spec.node_name);
                    std::ofstream(std::format("{}/{:02}_{}_cone_{}.json", dir, number, passName, spec.node_name))
                        << module.dfg->toJsonCone(node);
                }
            }
        };

        // Helper: dump flops from all modules in the hierarchy into a stream
        std::function<void(const ResolvedModule&, std::ostream&)> dumpFlopsRecursive =
            [&](const ResolvedModule& mod, std::ostream& f) {
                if (!mod.flops.empty()) {
                    f << "=== " << mod.name << " ===\n";
                    for (const auto& flop : mod.flops)
                        flop.print(f);
                }
                for (const auto& sub : mod.hierarchyInstantiation)
                    dumpFlopsRecursive(sub, f);
            };

        runPass(0, "elaboration", []{});
        // Inline all submodule DFGs into this module's flat DFG
        runPass(1, "dfg_inline", [&]{ inlineDFGs(module); });
        runPass(2, "concat_cleanup", [&]{ cleanupConcats(*module.dfg); });
        runPass(3, "constant_fold", [&]{ constantFold(*module.dfg); });
        runPass(4, "type_propagation", [&]{ propagateTypes(*module.dfg); });
        runPass(5, "condition_normalization", [&]{ normalizeConditions(*module.dfg); });
        runPass(6, "constant_fold", [&]{ constantFold(*module.dfg); });
        runPass(7, "condition_normalization", [&]{ normalizeConditions(*module.dfg); });
        runPass(8, "constant_fold", [&]{ constantFold(*module.dfg); });
        runPass(9, "io_domains_set", [&]{
            // Call setIODomains for this module and all submodules recursively
            std::function<void(ResolvedModule&)> setDomains = [&](ResolvedModule& mod) {
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
            // Collect declared signal nodes from every level of the hierarchy.
            // After dfg_inline, these nodes live in module.dfg->nodes but are
            // absent from module.dfg->signals, so DCE would otherwise remove them.
            std::unordered_set<DFGNode*> declaredSignals;
            std::function<void(const ResolvedModule&)> collect = [&](const ResolvedModule& mod) {
                for (const auto& [name, sig] : mod.signals)
                    if (sig.dfg_node) declaredSignals.insert(sig.dfg_node);
                for (const auto& sub : mod.hierarchyInstantiation)
                    collect(sub);
            };
            collect(module);
            eliminateDeadCode(*module.dfg, declaredSignals);
        });
        module.dfg->validateNoOrphans();
        runPass(12, "domains_propagate_and_check", [&]{ domainsPropagateAndCheck(module); });
        {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            std::ofstream f(std::format("{}/12_domains_propagate_flops.txt", dir));
            dumpFlopsRecursive(module, f);
        }
        computeComboDeps(module);
        validateNoCombLoops(module);

        if (simConfig && !simConfig->debug_dfg_nodes.empty()) {
            std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
            for (const auto& spec : simConfig->debug_dfg_nodes) {
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

    {
        std::string dir = DEBUG_OUTPUT_DIR + "/" + topModule.name;
        std::filesystem::create_directories(dir);
        std::ofstream(dir + "/hierarchy.json") << topModule.toJson();
    }

    std::cout << "----------------------------------------" << std::endl;
    std::cout << "\nResolved IR:" << std::endl;
    std::cout << "========================================" << std::endl;

    topModule.print();
    std::cout << std::endl;

    // Run simulation if requested
    if (simConfig) {
        std::cout << "========================================" << std::endl;
        std::cout << "Running simulation..." << std::endl;
        std::cout << "========================================" << std::endl;

        Simulator sim(topModule, *simConfig);
        sim.run();
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Compilation completed successfully!" << std::endl;
    std::cout << "Found " << ir.modules.size() << " module(s)." << std::endl;

    } catch (const CompilerError& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
