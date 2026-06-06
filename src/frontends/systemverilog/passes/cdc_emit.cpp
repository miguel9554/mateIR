#include "frontends/systemverilog/passes/cdc_emit.h"

#include "frontends/systemverilog/passes/top_io_domains_emit.h"

#include <filesystem>
#include <format>
#include <sstream>

namespace mate {

void emitInferredCdcYamls(const FrontendDomainFacts& domainFacts,
                           const std::string& outputDir) {
    for (const auto& [key, moduleFacts] : domainFacts.modules) {
        if (moduleFacts.cdc.synchronizer_flops.empty()) continue;

        std::ostringstream yaml;
        yaml << "synchronizer_flops:\n";
        for (const auto& flop : moduleFacts.cdc.synchronizer_flops)
            yaml << "  - " << flop << "\n";

        const std::string path = std::format(
            "{}/{}.inferred.cdc.yaml", outputDir, key.module_name);
        writeAtomically(path, yaml.str());
    }
}

} // namespace mate
