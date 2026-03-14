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
#include "passes/flop_resolve.h"
#include "passes/combo_deps.h"
#include "passes/domain_resolve.h"
#include "passes/type_propagation.h"
#include "sim/simulator.h"
#include "util/debug.h"
#include "util/source_loc.h"

#include "slang/syntax/SyntaxTree.h"

using namespace slang::syntax;
using namespace custom_hdl;

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [--passes <1|2>] <verilog_file>" << std::endl;
    std::cerr << "       " << progName << " --simulate --top <module>"
              << " --inputs-dir <dir> --output-dir <dir>" << std::endl;
    std::cerr << "           [--flops-initial <random|zeros|ones>] [--flops-seed <n>]" << std::endl;
    std::cerr << "           [--debug-nodes <n1,n2,...>] [--params <K=V,K=V,...>]" << std::endl;
    std::cerr << "           <source1.v> [source2.v ...]" << std::endl;
    std::cerr << "\nOptions:" << std::endl;
    std::cerr << "  --passes <1|2>            Number of passes (default: 1)" << std::endl;
    std::cerr << "  --simulate                Run cycle-based simulation" << std::endl;
    std::cerr << "  --top <module>            Top module name (simulate mode)" << std::endl;
    std::cerr << "  --inputs-dir <dir>        Directory containing input stimuli files" << std::endl;
    std::cerr << "  --output-dir <dir>        Directory for simulation output" << std::endl;
    std::cerr << "  --flops-initial <mode>    Flop init: random (default), zeros, ones" << std::endl;
    std::cerr << "  --flops-seed <n>          Seed for random flop initialization" << std::endl;
    std::cerr << "  --debug-nodes <n1,n2,...> Comma-separated node names for debug DOTs" << std::endl;
    std::cerr << "  --params <K=V,K=V,...>    Comma-separated parameter overrides" << std::endl;
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
    int numPasses = 1;
    bool simulateMode = false;
    std::string topModule;
    std::string inputsDir;
    std::string outputDir;
    std::string flopsInitialStr;
    std::string flopsSeedStr;
    std::string debugNodesStr;
    std::string paramsStr;
    std::vector<std::string> sourceFiles;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--passes") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: --passes requires an argument" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            numPasses = std::atoi(argv[++i]);
            if (numPasses < 1 || numPasses > 2) {
                std::cerr << "ERROR: --passes must be 1 or 2" << std::endl;
                printUsage(argv[0]);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--simulate") == 0) {
            simulateMode = true;
            numPasses = 2;
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
            cfg.debug_dfg_nodes = splitComma(debugNodesStr);
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
    std::cout << "Passes: " << numPasses << std::endl;
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

    // Pass 1: Build unresolved IR
    std::cout << "\nPass 1: Building unresolved IR..." << std::endl;
    auto modules = buildIR(*tree);

    std::cout << "----------------------------------------" << std::endl;
    std::cout << "\nUnresolved IR (Pass 1):" << std::endl;
    std::cout << "========================================" << std::endl;

    for (const auto& module : modules) {
        module->print();
        std::cout << std::endl;
    }

    // Pass 2: Resolution (optional)
    if (numPasses >= 2) {
        std::cout << "========================================" << std::endl;
        std::cout << "\nPass 2: Resolving types and expressions..." << std::endl;

        std::cout << "========================================" << std::endl;
        std::cout << "Performing elaboration..." << std::endl;
        std::cout << "========================================" << std::endl;

        // Build parameter context for top module if sim config provides parameters
        auto resolvedModules = [&]() {
            if (simConfig && !simConfig->parameters.empty()) {
                ParameterContext topParams;
                for (const auto& [name, value] : simConfig->parameters) {
                    topParams.values[name] = static_cast<int>(value);
                }
                return resolveModules(modules, tree->sourceManager(),
                                      simConfig->top_module, topParams);
            }
            return resolveModules(modules, tree->sourceManager());
        }();

        // Run the full pass pipeline on a module (and recursively on its submodules)
        std::function<void(ResolvedModule&)> runPipeline = [&](ResolvedModule& module) {
            if (!module.dfg) return;

            // Run pipeline on submodules first (bottom-up, so combo_deps are ready)
            for (auto& sub : module.hierarchyInstantiation) {
                runPipeline(sub);
            }

            std::cout << "========================================" << std::endl;
            std::cout << "Module: " << module.name << std::endl;
            std::cout << "========================================" << std::endl;

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
                    std::ofstream(std::format("{}/{}_{}_ERROR.dot", dir, number, passName))
                        << module.dfg->toDot(passName + "_ERROR", errorNodes);
                    throw;
                }

                std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
                std::filesystem::create_directories(dir);
                std::ofstream(std::format("{}/{}_{}.dot", dir, number, passName)) << module.dfg->toDot(passName);
                std::ofstream(std::format("{}/{}_{}.json", dir, number, passName)) << module.dfg->toJson();
            };

            runPass(0, "elaboration", []{});
            runPass(1, "concat_cleanup", [&]{ cleanupConcats(*module.dfg); });
            runPass(2, "type_propagation", [&]{ propagateTypes(*module.dfg); });
            runPass(3, "condition_normalization", [&]{ normalizeConditions(*module.dfg); });
            runPass(4, "constant_fold", [&]{ constantFold(*module.dfg); });
            runPass(5, "dce", [&]{ eliminateDeadCode(*module.dfg); });
            module.dfg->validateNoOrphans();
            runPass(6, "flop_resolve", [&]{ resolveFlops(module); });
            runPass(7, "domain_resolve", [&]{ resolveDomains(module); });
            computeComboDepsBU(module);
            validateNoCombLoops(module);

            if (simConfig && !simConfig->debug_dfg_nodes.empty()) {
                std::string dir = DEBUG_OUTPUT_DIR + "/" + module.name;
                std::filesystem::create_directories(dir);
                for (const auto& nodeName : simConfig->debug_dfg_nodes) {
                    const DFGNode* node = nullptr;
                    if (auto it = module.dfg->signals.find(nodeName); it != module.dfg->signals.end())
                        node = it->second;
                    else if (auto it = module.dfg->outputs.find(nodeName); it != module.dfg->outputs.end())
                        node = it->second;
                    else
                        throw CompilerError(std::format(
                            "debug_dfg_nodes: node '{}' not found in signals or outputs", nodeName));

                    std::ofstream(std::format("{}/cone_{}.dot", dir, nodeName))
                        << module.dfg->toDotCone(node, "cone_" + nodeName);
                    std::ofstream(std::format("{}/cone_{}.json", dir, nodeName))
                        << module.dfg->toJsonCone(node);
                }
            }
        };

        // Optimization passes
        for (auto& module : resolvedModules) {
            runPipeline(module);
        }

        std::cout << "----------------------------------------" << std::endl;
        std::cout << "\nResolved IR (Pass 2):" << std::endl;
        std::cout << "========================================" << std::endl;

        for (const auto& module : resolvedModules) {
            module.print();
            std::cout << std::endl;
        }

        // Run simulation if requested
        if (simConfig) {
            std::cout << "========================================" << std::endl;
            std::cout << "Running simulation..." << std::endl;
            std::cout << "========================================" << std::endl;

            // Find the top module
            const ResolvedModule* topModule = nullptr;
            for (const auto& mod : resolvedModules) {
                if (mod.name == simConfig->top_module) {
                    topModule = &mod;
                    break;
                }
            }
            if (!topModule) {
                throw CompilerError(std::format(
                    "Simulator: top module '{}' not found in resolved modules",
                    simConfig->top_module));
            }

            Simulator sim(*topModule, *simConfig);
            sim.run();
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Compilation completed successfully!" << std::endl;
    std::cout << "Found " << modules.size() << " module(s)." << std::endl;

    } catch (const CompilerError& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
