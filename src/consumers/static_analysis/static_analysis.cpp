#include "consumers/static_analysis/static_analysis.h"

#include <algorithm>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace mate {

namespace {

const char* edgeName(edge_t edge) {
    switch (edge) {
        case POSEDGE: return "posedge";
        case NEGEDGE: return "negedge";
    }
    return "unknown";
}

const char* signalNamespaceName(SignalNamespace ns) {
    switch (ns) {
        case SignalNamespace::Input: return "input";
        case SignalNamespace::Output: return "output";
        case SignalNamespace::Internal: return "internal";
        case SignalNamespace::FlopQ: return "flop_q";
        case SignalNamespace::FlopD: return "flop_d";
    }
    return "unknown";
}

std::string instancePathName(const InstancePath& path) {
    if (path.elems.empty()) return "<top>";
    std::string result;
    for (size_t i = 0; i < path.elems.size(); ++i) {
        if (i) result += ".";
        result += path.elems[i];
    }
    return result;
}

std::string hierSignalRefName(const HierSignalRef& ref) {
    return std::format("{} {}.{}",
                       signalNamespaceName(ref.ns),
                       instancePathName(ref.instance_path),
                       ref.name);
}

const ClockDomain* findClockDomain(const MateIR& ir, ClockId id) {
    auto it = std::find_if(ir.clocks.begin(), ir.clocks.end(), [&](const ClockDomain& domain) {
        return domain.id == id;
    });
    return it == ir.clocks.end() ? nullptr : &*it;
}

const ResetDomain* findResetDomain(const MateIR& ir, ResetId id) {
    auto it = std::find_if(ir.resets.begin(), ir.resets.end(), [&](const ResetDomain& domain) {
        return domain.id == id;
    });
    return it == ir.resets.end() ? nullptr : &*it;
}

std::string clockDomainLabel(const MateIR& ir, ClockId id) {
    if (id == InvalidClockId) return "invalid";
    if (const auto* domain = findClockDomain(ir, id)) {
        return std::format("({}, {})",
                           domain->display_name,
                           edgeName(domain->edge));
    }
    return "unknown";
}

std::string resetDomainLabel(const MateIR& ir, ResetId id) {
    if (id == InvalidResetId) return "invalid";
    if (const auto* domain = findResetDomain(ir, id)) {
        return std::format("({}, {})",
                           domain->display_name,
                           edgeName(domain->active_edge));
    }
    return "unknown";
}

std::string moduleDisplayName(const Module& module) {
    if (module.instance_name.empty()) return module.name;
    return std::format("{} ({})", module.instance_name, module.name);
}

bool debugPathMatches(const std::string& currentPath,
                      const std::string& modulePath) {
    return modulePath.empty() ||
           currentPath == modulePath ||
           currentPath.ends_with("." + modulePath);
}

std::string debugInputLabel(const DFGNode* node, size_t index) {
    switch (node->kind()) {
        case DFGOp::MUX:
            if (index == 0) return "sel";
            if (index > 0 && index - 1 < node->muxValues().size()) {
                return std::format("d[{}]", node->muxValues()[index - 1]);
            }
            break;
        case DFGOp::SLICE:
            if (index == 0) return "src";
            if (index == 1) return "hi";
            if (index == 2) return "lo";
            break;
        default:
            break;
    }
    return std::format("arg{}", index);
}

std::string formatNodeHeader(const DFGNode* node) {
    std::ostringstream out;
    out << "node: " << node->str() << "\n";
    out << "op: " << to_string(node->kind()) << "\n";
    if (node->hasType()) {
        out << "type: width=" << node->type->width
            << ", signed=" << (node->type->isSigned() ? "true" : "false") << "\n";
    }
    if (node->loc) {
        out << "loc: " << node->loc->str() << "\n";
    }
    return out.str();
}

std::string formatInputEdge(const DFGNode* user,
                            size_t index,
                            const DFGOutput& input) {
    std::ostringstream out;
    out << "[" << index << "] " << debugInputLabel(user, index)
        << " -> " << input.node->str();
    if (input.port != 0) out << " (port " << input.port << ")";
    if (input.node->hasType()) {
        out << " [w=" << input.node->type->width
            << ", s=" << (input.node->type->isSigned() ? "1" : "0") << "]";
    }
    return out.str();
}

std::string dependencyKindLabel(const DFGNode* node) {
    if (node->name.ends_with(".q")) return "flop_q";
    return "top_input";
}

std::string dependencyDisplayName(const DFGNode* node) {
    if (node->instance_path.empty()) return node->name;
    return node->instance_path + "." + node->name;
}

struct DebugNodeMatch {
    const Module* module = nullptr;
    const DFGNode* node = nullptr;
};

struct LocalDependencyRef {
    std::string kind;
    std::string name;
    const DFGNode* node = nullptr;
};

struct LocalDependencyWalkResult {
    std::vector<LocalDependencyRef> deps;
    std::vector<const DFGNode*> escaped_inputs;
};

std::vector<const DFGNode*> collectGlobalDependencies(const DFGNode* root) {
    std::set<const DFGNode*> visited;
    std::queue<const DFGNode*> q;
    std::map<std::string, const DFGNode*> depsByName;

    q.push(root);
    visited.insert(root);

    while (!q.empty()) {
        const DFGNode* node = q.front();
        q.pop();

        if (node->kind() == DFGOp::INPUT) {
            depsByName.emplace(dependencyDisplayName(node), node);
            continue;
        }

        DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
            if (visited.insert(input.node).second) {
                q.push(input.node);
            }
        });
    }

    std::vector<const DFGNode*> deps;
    deps.reserve(depsByName.size());
    for (const auto& [_, node] : depsByName) deps.push_back(node);
    return deps;
}

LocalDependencyWalkResult collectLocalDependencies(const Module& module,
                                                   const DFGNode* root) {
    std::map<const DFGNode*, LocalDependencyRef> stopSet;

    forEachInputNode(module, [&](const ModuleNode& input) {
        for (const auto& leaf : moduleNodeLeafRefs(input)) {
            if (!leaf.node) continue;
            stopSet.emplace(leaf.node, LocalDependencyRef{
                .kind = "module_input",
                .name = leaf.leaf_name,
                .node = leaf.node,
            });
        }
    });
    for (const auto& flop : module.flops) {
        for (const auto& leaf : flopQLeafRefs(flop)) {
            if (!leaf.node) continue;
            stopSet.emplace(leaf.node, LocalDependencyRef{
                .kind = "local_flop_q",
                .name = leaf.leaf_name,
                .node = leaf.node,
            });
        }
    }

    std::set<const DFGNode*> visited;
    std::queue<const DFGNode*> q;
    std::map<std::string, LocalDependencyRef> depsByName;
    std::map<std::string, const DFGNode*> escapedByName;

    q.push(root);
    visited.insert(root);

    while (!q.empty()) {
        const DFGNode* node = q.front();
        q.pop();

        if (auto it = stopSet.find(node); it != stopSet.end()) {
            depsByName.emplace(it->second.name, it->second);
            continue;
        }
        if (node->kind() == DFGOp::INPUT) {
            escapedByName.emplace(dependencyDisplayName(node), node);
            continue;
        }

        DFGTraversal::forEachInput(node, [&](size_t, const DFGOutput& input) {
            if (visited.insert(input.node).second) {
                q.push(input.node);
            }
        });
    }

    LocalDependencyWalkResult result;
    result.deps.reserve(depsByName.size());
    for (const auto& [_, dep] : depsByName) result.deps.push_back(dep);
    result.escaped_inputs.reserve(escapedByName.size());
    for (const auto& [_, node] : escapedByName) result.escaped_inputs.push_back(node);
    return result;
}

DebugNodeMatch findDebugNodeInIR(const Module& topModule,
                                 const DebugNodeSpec& spec) {
    DebugNodeMatch result;
    std::function<void(const Module&, const std::string&)> recurse =
        [&](const Module& module, const std::string& currentPath) {
            if (result.node) return;

            if (debugPathMatches(currentPath, spec.module_path)) {
                if (const auto* node = findModuleDebugLeafNode(module, spec.node_name)) {
                    result = DebugNodeMatch{
                        .module = &module,
                        .node = node,
                    };
                    return;
                }
            }

            for (const auto& sub : module.hierarchyInstantiation) {
                recurse(sub, currentPath + "." + sub.instance_name);
                if (result.node) return;
            }
        };

    recurse(topModule, topModule.name);
    return result;
}

void printRequestedNodeDeps(const MateIR& ir,
                            const std::vector<DebugNodeSpec>& specs,
                            std::ostream& out) {
    if (specs.empty()) return;

    out << "  dfg_node_dependencies:\n";
    for (const auto& spec : specs) {
        const DebugNodeMatch match = findDebugNodeInIR(ir.top, spec);
        if (!match.node || !match.module) {
            throw CompilerError(std::format(
                "debug_dfg_node_deps: node '{}' not found in any module outputs, signals, or flop .d/.q leaves",
                spec.node_name));
        }

        out << "    request: ";
        if (!spec.module_path.empty()) out << spec.module_path << ":";
        out << spec.node_name << "\n";

        std::istringstream header(formatNodeHeader(match.node));
        std::string line;
        while (std::getline(header, line)) {
            out << "      " << line << "\n";
        }

        size_t directInputCount = 0;
        DFGTraversal::forEachInput(match.node, [&](size_t index, const DFGOutput& input) {
            if (directInputCount == 0) out << "      direct_inputs:\n";
            out << "        " << formatInputEdge(match.node, index, input) << "\n";
            ++directInputCount;
        });
        if (directInputCount == 0) {
            out << "      direct_inputs: (none)\n";
        }

        const auto deps = collectGlobalDependencies(match.node);
        if (deps.empty()) {
            out << "      global_dependencies: (none)\n";
        } else {
            out << "      global_dependencies:\n";
            for (const DFGNode* dep : deps) {
                out << "        " << dependencyKindLabel(dep)
                    << " " << dependencyDisplayName(dep);
                if (dep->hasType()) {
                    out << " [w=" << dep->type->width
                        << ", s=" << (dep->type->isSigned() ? "1" : "0") << "]";
                }
                out << "\n";
            }
        }

        const auto local = collectLocalDependencies(*match.module, match.node);
        if (local.deps.empty()) {
            out << "      local_dependencies: (none)\n";
        } else {
            out << "      local_dependencies:\n";
            for (const auto& dep : local.deps) {
                out << "        " << dep.kind << " " << dep.name;
                if (dep.node && dep.node->hasType()) {
                    out << " [w=" << dep.node->type->width
                        << ", s=" << (dep.node->type->isSigned() ? "1" : "0") << "]";
                }
                out << "\n";
            }
        }

        if (!local.escaped_inputs.empty()) {
            std::ostringstream escaped;
            bool first = true;
            for (const DFGNode* dep : local.escaped_inputs) {
                if (!first) escaped << ", ";
                first = false;
                escaped << dependencyKindLabel(dep) << " " << dependencyDisplayName(dep);
            }
            throw CompilerError(std::format(
                "debug_dfg_node_deps: local dependency reduction for '{}' escaped module '{}'; reached nonlocal graph inputs: {}",
                spec.node_name, match.module->name, escaped.str()));
        }
    }
}

void collectModule(const Module& module, StaticAnalysisSummary& summary) {
    summary.modules++;
    forEachInputNode(module, [&](const ModuleNode&) { summary.inputs++; });
    forEachOutputNode(module, [&](const ModuleNode&) { summary.outputs++; });
    forEachInternalNode(module, [&](const ModuleNode&) { summary.signals++; });
    summary.flops += module.flops.size();
    if (module.dfg) summary.dfg_nodes += module.dfg->nodes.size();
    validateNoCombLoops(module);
    for (const auto& sub : module.hierarchyInstantiation)
        collectModule(sub, summary);
}

void printDomainRegistry(const MateIR& ir, std::ostream& out) {
    out << "  clock_domains:\n";
    if (ir.clocks.empty()) {
        out << "    none\n";
    } else {
        for (const auto& clock : ir.clocks) {
            out << "    " << clock.id.value
                << ": " << clock.display_name
                << " edge=" << edgeName(clock.edge)
                << " source=" << hierSignalRefName(clock.source)
                << "\n";
        }
    }

    out << "  reset_domains:\n";
    if (ir.resets.empty()) {
        out << "    none\n";
    } else {
        for (const auto& reset : ir.resets) {
            out << "    " << reset.id.value
                << ": " << reset.display_name
                << " active_edge=" << edgeName(reset.active_edge)
                << " source=" << hierSignalRefName(reset.source)
                << "\n";
        }
    }
}

void printDomainSet(const MateIR& ir,
                    const std::set<ClockId>& clocks,
                    std::ostream& out,
                    const std::string& indent) {
    if (clocks.empty()) return;

    out << indent << "clock_domains:\n";
    for (ClockId clock : clocks)
        out << indent << "\t" << clockDomainLabel(ir, clock) << "\n";
}

void printDomainSet(const MateIR& ir,
                    const std::set<ResetId>& resets,
                    std::ostream& out,
                    const std::string& indent) {
    if (resets.empty()) return;

    out << indent << "reset_domains:\n";
    for (ResetId reset : resets)
        out << indent << "\t" << resetDomainLabel(ir, reset) << "\n";
}

void printModuleDomains(const MateIR& ir,
                        const Module& module,
                        std::ostream& out,
                        size_t depth = 0) {
    std::string indent(depth + 1, '\t');
    std::string childIndent(depth + 2, '\t');
    out << indent << "module " << moduleDisplayName(module) << ":\n";

    std::set<ClockId> clocks;
    std::set<ResetId> resets;
    for (const auto& flop : module.flops) {
        if (flop.clock_domain != InvalidClockId)
            clocks.insert(flop.clock_domain);
        for (ResetId reset : flop.reset_domains.ids) {
            if (reset != InvalidResetId)
                resets.insert(reset);
        }
    }

    printDomainSet(ir, clocks, out, childIndent);
    printDomainSet(ir, resets, out, childIndent);

    for (const auto& sub : module.hierarchyInstantiation) {
        printModuleDomains(ir, sub, out, depth + 1);
    }
}

} // namespace

StaticAnalysisConsumer::StaticAnalysisConsumer(std::ostream& out,
                                               std::vector<DebugNodeSpec> debug_dfg_node_deps)
    : out_(out),
      debug_dfg_node_deps_(std::move(debug_dfg_node_deps)) {}

std::string StaticAnalysisConsumer::name() const {
    return "static-analysis";
}

void StaticAnalysisConsumer::consume(const MateIR& ir) {
    auto summary = analyzeMateIR(ir);
    out_ << "mateir static analysis\n";
    out_ << "  top: " << ir.top.name << "\n";
    out_ << "  modules: " << summary.modules << "\n";
    out_ << "  inputs: " << summary.inputs << "\n";
    out_ << "  outputs: " << summary.outputs << "\n";
    out_ << "  signals: " << summary.signals << "\n";
    out_ << "  flops: " << summary.flops << "\n";
    out_ << "  dfg_nodes: "              << summary.dfg_nodes     << "\n";
    printDomainRegistry(ir, out_);
    out_ << "  domain_usage:\n";
    printModuleDomains(ir, ir.top, out_);
    printRequestedNodeDeps(ir, debug_dfg_node_deps_, out_);
}

StaticAnalysisSummary analyzeMateIR(const MateIR& ir) {
    StaticAnalysisSummary summary;
    collectModule(ir.top, summary);
    return summary;
}

} // namespace mate
