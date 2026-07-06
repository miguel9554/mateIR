#include "simulator_consumer.h"
#include "consumers/static_analysis/static_analysis.h"
#include "dpi_codegen/dpi_codegen.h"
#include "dpi_codegen/dpi_lib_link.h"
#include "frontends/systemverilog/systemverilog_frontend.h"
#include "sim/runtime_model.h"
#include "util/source_loc.h"

#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace mate;

namespace {

void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [options] <source1.v> [source2.v ...]\n"
              << "\nOptions:\n"
              << "  --frontend <name>         HDL frontend: systemverilog (default)\n"
              << "  --top <module>            Top module name (required for multi-file designs)\n"
              << "  --domains <file>          Top-level domain YAML file\n"
              << "  --infer-top-domains       Infer top-level clocks/resets instead of loading --domains\n"
              << "  --infer-synchronizers     Infer CDC synchronizer flops instead of loading .cdc.yaml sidecars\n"
              << "  --emit-inferred-domains <file>\n"
              << "                            Write inferred top-level domains YAML (infer mode only)\n"
              << "  --emit-inferred-synchronizers <dir>\n"
              << "                            Write inferred CDC synchronizer YAMLs to dir (infer mode only)\n"
              << "  --analyze                 Run static analysis consumer on mateir\n"
              << "  --simulate                Run cycle-based simulation consumer\n"
              << "  --dpi-lib                 Generate DPI glue/native model and compile them into\n"
              << "                            a single static library any SV simulator can link\n"
              << "  --inputs-dir <dir>        Directory containing input stimuli files\n"
              << "  --output-dir <dir>        Directory for simulation output, or for --dpi-lib\n"
              << "                            generated .cpp/.sv output\n"
              << "  --module-name <name>      SV module name for --dpi-lib generated wrapper\n"
              << "  --function-prefix <name>  C function name prefix for --dpi-lib generated glue\n"
              << "  --dpi-out-lib <path>      Output path for the --dpi-lib compiled static library\n"
              << "  --dpi-include-dirs <d1,d2,...>\n"
              << "                            Extra -I directories for compiling --dpi-lib output\n"
              << "  --dpi-link-libs <lib1.a,lib2.a,...>\n"
              << "                            Static libraries whose objects are folded into the\n"
              << "                            --dpi-lib output archive\n"
              << "  --cxx <path>              C++ compiler for --dpi-lib (default: c++)\n"
              << "  --ar <path>               Archiver for --dpi-lib (default: ar)\n"
              << "  --flops-initial <mode>    Flop init: random (default), zeros, ones\n"
              << "  --flops-seed <n>          Seed for random flop initialization\n"
              << "  --debug-nodes <n1,n2,...> Comma-separated node names for debug DOTs\n"
              << "  --debug-node-deps <...>   Comma-separated node names to dump direct DFG inputs\n"
              << "  --debug-node-paths <...>  Comma-separated SOURCE=TARGET queries to dump one dependency chain\n"
              << "  --dump-passes             Dump per-pass DFG .dot/.json and final DFG dump\n"
              << "  --params <K=V,K=V,...>    Comma-separated top parameter overrides\n";
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, ',')) {
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

void parseParams(const std::string& paramsStr, std::map<std::string, int64_t>& out) {
    if (paramsStr.empty()) return;
    for (const auto& kv : splitComma(paramsStr)) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) {
            throw CompilerError("--params entry '" + kv + "' must be KEY=VALUE");
        }
        out[kv.substr(0, eq)] = std::stoll(kv.substr(eq + 1));
    }
}

std::vector<DebugNodeSpec> parseDebugNodes(const std::string& debugNodesStr) {
    std::vector<DebugNodeSpec> specs;
    if (debugNodesStr.empty()) return specs;
    for (const auto& entry : splitComma(debugNodesStr)) {
        auto colon = entry.find(':');
        if (colon == std::string::npos)
            specs.push_back({"", entry});
        else
            specs.push_back({entry.substr(0, colon), entry.substr(colon + 1)});
    }
    return specs;
}

std::vector<DebugNodePathSpec> parseDebugNodePaths(const std::string& debugPathsStr) {
    std::vector<DebugNodePathSpec> specs;
    if (debugPathsStr.empty()) return specs;
    for (const auto& entry : splitComma(debugPathsStr)) {
        auto eq = entry.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= entry.size()) {
            throw CompilerError(
                "--debug-node-paths entries must have the form SOURCE=TARGET");
        }

        DebugNodePathSpec spec;
        spec.source_name = entry.substr(0, eq);

        const std::string target = entry.substr(eq + 1);
        auto colon = target.find(':');
        if (colon == std::string::npos)
            spec.target = {"", target};
        else
            spec.target = {target.substr(0, colon), target.substr(colon + 1)};

        specs.push_back(std::move(spec));
    }
    return specs;
}

std::unique_ptr<Frontend> makeFrontend(const std::string& frontendName) {
    if (frontendName == "systemverilog" || frontendName == "sv" || frontendName == "verilog") {
        return std::make_unique<SystemVerilogFrontend>();
    }
    throw CompilerError("Unknown frontend: " + frontendName);
}

} // namespace

int main(int argc, char** argv) {
    bool simulateMode = false;
    bool analyzeMode = false;
    bool dpiLibMode = false;
    bool dumpPasses = false;
    std::string frontendName = "systemverilog";
    std::string topModule;
    std::string inputsDir;
    std::string outputDir;
    std::string flopsInitialStr;
    std::string flopsSeedStr;
    std::string debugNodesStr;
    std::string debugNodeDepsStr;
    std::string debugNodePathsStr;
    std::string paramsStr;
    std::string emitInferredDomainsPath;
    std::string emitInferredSynchronizersDir;
    std::string moduleName;
    std::string functionPrefix;
    std::string dpiOutLibStr;
    std::string dpiIncludeDirsStr;
    std::string dpiLinkLibsStr;
    std::string cxxPath = "c++";
    std::string arPath = "ar";
    std::vector<std::string> domainFiles;
    std::vector<std::string> sourceFiles;
    TopDomainMode topDomainMode = TopDomainMode::Yaml;
    bool inferSynchronizers = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--simulate") == 0) {
            simulateMode = true;
        } else if (std::strcmp(argv[i], "--analyze") == 0) {
            analyzeMode = true;
        } else if (std::strcmp(argv[i], "--dpi-lib") == 0) {
            dpiLibMode = true;
        } else if (std::strcmp(argv[i], "--infer-top-domains") == 0) {
            topDomainMode = TopDomainMode::Infer;
        } else if (std::strcmp(argv[i], "--infer-synchronizers") == 0) {
            inferSynchronizers = true;
        } else if (std::strcmp(argv[i], "--dump-passes") == 0) {
            dumpPasses = true;
        } else if (std::strcmp(argv[i], "--frontend") == 0 ||
                   std::strcmp(argv[i], "--top") == 0 ||
                   std::strcmp(argv[i], "--inputs-dir") == 0 ||
                   std::strcmp(argv[i], "--output-dir") == 0 ||
                   std::strcmp(argv[i], "--flops-initial") == 0 ||
                   std::strcmp(argv[i], "--flops-seed") == 0 ||
                   std::strcmp(argv[i], "--debug-nodes") == 0 ||
                   std::strcmp(argv[i], "--debug-node-deps") == 0 ||
                   std::strcmp(argv[i], "--debug-node-paths") == 0 ||
                   std::strcmp(argv[i], "--params") == 0 ||
                   std::strcmp(argv[i], "--emit-inferred-domains") == 0 ||
                   std::strcmp(argv[i], "--emit-inferred-synchronizers") == 0 ||
                   std::strcmp(argv[i], "--module-name") == 0 ||
                   std::strcmp(argv[i], "--function-prefix") == 0 ||
                   std::strcmp(argv[i], "--dpi-out-lib") == 0 ||
                   std::strcmp(argv[i], "--dpi-include-dirs") == 0 ||
                   std::strcmp(argv[i], "--dpi-link-libs") == 0 ||
                   std::strcmp(argv[i], "--cxx") == 0 ||
                   std::strcmp(argv[i], "--ar") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "ERROR: " << argv[i] << " requires an argument\n";
                printUsage(argv[0]);
                return 1;
            }
            std::string opt = argv[i];
            std::string value = argv[++i];
            if (opt == "--frontend") frontendName = value;
            else if (opt == "--top") topModule = value;
            else if (opt == "--inputs-dir") inputsDir = value;
            else if (opt == "--output-dir") outputDir = value;
            else if (opt == "--flops-initial") flopsInitialStr = value;
            else if (opt == "--flops-seed") flopsSeedStr = value;
            else if (opt == "--debug-nodes") debugNodesStr = value;
            else if (opt == "--debug-node-deps") debugNodeDepsStr = value;
            else if (opt == "--debug-node-paths") debugNodePathsStr = value;
            else if (opt == "--params") paramsStr = value;
            else if (opt == "--emit-inferred-domains") emitInferredDomainsPath = value;
            else if (opt == "--emit-inferred-synchronizers") emitInferredSynchronizersDir = value;
            else if (opt == "--module-name") moduleName = value;
            else if (opt == "--function-prefix") functionPrefix = value;
            else if (opt == "--dpi-out-lib") dpiOutLibStr = value;
            else if (opt == "--dpi-include-dirs") dpiIncludeDirsStr = value;
            else if (opt == "--dpi-link-libs") dpiLinkLibsStr = value;
            else if (opt == "--cxx") cxxPath = value;
            else if (opt == "--ar") arPath = value;
        } else if (std::strcmp(argv[i], "--domains") == 0) {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string_view arg(argv[i + 1]);
                if (!arg.ends_with(".yaml") && !arg.ends_with(".yml")) break;
                domainFiles.push_back(argv[++i]);
            }
            if (domainFiles.empty()) {
                std::cerr << "ERROR: --domains requires at least one .yaml file\n";
                printUsage(argv[0]);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            std::cerr << "ERROR: Unknown option: " << argv[i] << "\n";
            printUsage(argv[0]);
            return 1;
        } else {
            sourceFiles.push_back(argv[i]);
        }
    }

    try {
        if (sourceFiles.empty()) {
            printUsage(argv[0]);
            return 1;
        }
        if (sourceFiles.size() > 1 && topModule.empty()) {
            throw CompilerError("Multiple source files require --top <module>");
        }
        if (simulateMode) {
            if (topModule.empty()) throw CompilerError("--simulate requires --top <module>");
            if (inputsDir.empty()) throw CompilerError("--simulate requires --inputs-dir <dir>");
            if (outputDir.empty()) throw CompilerError("--simulate requires --output-dir <dir>");
        }
        if (dpiLibMode) {
            if (simulateMode) throw CompilerError("--dpi-lib cannot be combined with --simulate");
            if (topModule.empty()) throw CompilerError("--dpi-lib requires --top <module>");
            if (outputDir.empty()) throw CompilerError("--dpi-lib requires --output-dir <dir>");
            if (moduleName.empty()) throw CompilerError("--dpi-lib requires --module-name <name>");
            if (functionPrefix.empty()) {
                throw CompilerError("--dpi-lib requires --function-prefix <prefix>");
            }
            if (dpiOutLibStr.empty()) throw CompilerError("--dpi-lib requires --dpi-out-lib <path>");
        }
        if (topDomainMode == TopDomainMode::Infer && !domainFiles.empty()) {
            throw CompilerError("--infer-top-domains cannot be used with --domains");
        }
        if (!emitInferredDomainsPath.empty() && topDomainMode != TopDomainMode::Infer) {
            throw CompilerError("--emit-inferred-domains requires --infer-top-domains");
        }
        if (!emitInferredSynchronizersDir.empty() && !inferSynchronizers) {
            throw CompilerError("--emit-inferred-synchronizers requires --infer-synchronizers");
        }

        std::cout << "========================================\n";
        std::cout << "mate\n";
        std::cout << "========================================\n";
        std::cout << "Frontend: " << frontendName << "\n";
        std::cout << "Sources: ";
        for (size_t i = 0; i < sourceFiles.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << sourceFiles[i];
        }
        std::cout << "\n----------------------------------------\n";

        FrontendOptions frontendOptions;
        frontendOptions.source_files = sourceFiles;
        frontendOptions.infer_synchronizers = inferSynchronizers;
        if (!topModule.empty()) frontendOptions.top_module = topModule;
        frontendOptions.domain_files = domainFiles;
        frontendOptions.top_domain_mode = topDomainMode;
        if (!emitInferredDomainsPath.empty()) {
            frontendOptions.emit_inferred_domains_path = emitInferredDomainsPath;
        }
        if (!emitInferredSynchronizersDir.empty()) {
            frontendOptions.emit_inferred_synchronizers_dir = emitInferredSynchronizersDir;
        }
        frontendOptions.debug_dfg_nodes = parseDebugNodes(debugNodesStr);
        frontendOptions.dump_passes = dumpPasses;
        parseParams(paramsStr, frontendOptions.parameters);

        auto frontend = makeFrontend(frontendName);
        auto mateir = frontend->compile(frontendOptions);
        const size_t frontendModuleCount = mateir.frontend_module_count;

        if (!dpiLibMode) {
            std::cout << "----------------------------------------\n";
            std::cout << "\nmateir:\n";
            std::cout << "========================================\n";
            mateir.top.print();
            std::cout << "\n";
        }
        if (dumpPasses) mateir.top.dumpDfgFiles();

        if (analyzeMode) {
            std::cout << "========================================\n";
            std::cout << "Running static analysis...\n";
            std::cout << "========================================\n";
            StaticAnalysisConsumer analyzer(std::cout,
                                           parseDebugNodes(debugNodeDepsStr),
                                           parseDebugNodePaths(debugNodePathsStr));
            analyzer.consume(mateir);
        }

        if (simulateMode) {
            SimConfig simConfig;
            simConfig.source_files = sourceFiles;
            simConfig.top_module = topModule;
            simConfig.inputs_dir = inputsDir;
            simConfig.output_dir = outputDir;
            simConfig.parameters = frontendOptions.parameters;
            if (!flopsInitialStr.empty()) {
                if (flopsInitialStr == "random") simConfig.flops_initial = FlopsInitial::Random;
                else if (flopsInitialStr == "zeros") simConfig.flops_initial = FlopsInitial::AllZeros;
                else if (flopsInitialStr == "ones") simConfig.flops_initial = FlopsInitial::AllOnes;
                else throw CompilerError("--flops-initial must be random, zeros, or ones");
            }
            if (!flopsSeedStr.empty()) {
                simConfig.flops_initial_seed = std::stoull(flopsSeedStr);
            }

            std::cout << "========================================\n";
            std::cout << "Running simulation...\n";
            std::cout << "========================================\n";
            RtlRuntimeModel runtimeModel(std::move(mateir));
            SimulatorConsumer simulator(std::move(simConfig));
            simulator.consume(runtimeModel);
        }

        if (dpiLibMode) {
            std::cout << "========================================\n";
            std::cout << "Generating DPI glue and native model...\n";
            std::cout << "========================================\n";
            RtlRuntimeModel runtimeModel(std::move(mateir));

            DpiCodegenConfig dpiConfig;
            dpiConfig.top_module = topModule;
            dpiConfig.sources = sourceFiles;
            dpiConfig.out_dir = outputDir;
            dpiConfig.module_name = moduleName;
            dpiConfig.function_prefix = functionPrefix;
            DpiCodegenOutput codegenOutput = generateDpiCodegen(dpiConfig, runtimeModel);

            std::cout << "Compiling into " << dpiOutLibStr << "...\n";
            DpiLibLinkConfig linkConfig;
            linkConfig.sources.push_back(codegenOutput.dpi_cpp);
            linkConfig.sources.insert(linkConfig.sources.end(),
                                      codegenOutput.model_cpps.begin(),
                                      codegenOutput.model_cpps.end());
            linkConfig.out_lib = dpiOutLibStr;
            linkConfig.cxx = cxxPath;
            linkConfig.ar = arPath;
            for (const auto& dir : splitComma(dpiIncludeDirsStr)) {
                linkConfig.include_dirs.emplace_back(dir);
            }
            for (const auto& lib : splitComma(dpiLinkLibsStr)) {
                linkConfig.link_libs.emplace_back(lib);
            }
            linkDpiLib(linkConfig);
        }

        std::cout << "========================================\n";
        std::cout << "Compilation completed successfully\n";
        std::cout << "Found " << frontendModuleCount << " module(s).\n";
    } catch (const CompilerError& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
