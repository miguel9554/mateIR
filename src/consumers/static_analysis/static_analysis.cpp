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
#include <unordered_map>
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

struct HierarchyPoint {
    std::string kind;
    std::string path;
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
    for (const auto& sub : module.hierarchyInstantiation) {
        forEachOutputNode(sub, [&](const ModuleNode& output) {
            for (const auto& leaf : moduleNodeLeafRefs(output)) {
                if (!leaf.node) continue;
                stopSet.emplace(leaf.node, LocalDependencyRef{
                    .kind = "submodule_output",
                    .name = sub.instance_name.empty()
                        ? leaf.leaf_name
                        : sub.instance_name + "." + leaf.leaf_name,
                    .node = leaf.node,
                });
            }
        });
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

const DFGNode* findGlobalSourceNode(const Module& topModule,
                                    const std::string& sourceName) {
    if (!topModule.dfg) return nullptr;
    for (const auto& node : topModule.dfg->nodes) {
        if (node->kind() != DFGOp::INPUT) continue;
        if (dependencyDisplayName(node.get()) == sourceName) return node.get();
    }
    return nullptr;
}

std::string modulePathWithLeaf(const std::string& modulePath,
                               const std::string& leafName) {
    return modulePath.empty() ? leafName : modulePath + "." + leafName;
}

void addHierarchyPoint(std::unordered_map<const DFGNode*, std::vector<HierarchyPoint>>& points,
                       const DFGNode* node,
                       std::string kind,
                       std::string path) {
    if (!node) return;
    points[node].push_back(HierarchyPoint{
        .kind = std::move(kind),
        .path = std::move(path),
    });
}

void collectHierarchyPoints(const Module& module,
                            const std::string& currentPath,
                            std::unordered_map<const DFGNode*, std::vector<HierarchyPoint>>& points) {
    forEachInputNode(module, [&](const ModuleNode& input) {
        for (const auto& leaf : moduleNodeLeafRefs(input)) {
            addHierarchyPoint(points, leaf.node, "module_input",
                              modulePathWithLeaf(currentPath, leaf.leaf_name));
        }
    });
    forEachOutputNode(module, [&](const ModuleNode& output) {
        for (const auto& leaf : moduleNodeLeafRefs(output)) {
            addHierarchyPoint(points, leaf.node, "module_output",
                              modulePathWithLeaf(currentPath, leaf.leaf_name));
        }
    });
    forEachInternalNode(module, [&](const ModuleNode& signal) {
        for (const auto& leaf : moduleNodeLeafRefs(signal)) {
            addHierarchyPoint(points, leaf.node, "module_internal",
                              modulePathWithLeaf(currentPath, leaf.leaf_name));
        }
    });
    for (const auto& flop : module.flops) {
        for (const auto& leaf : flopQLeafRefs(flop)) {
            addHierarchyPoint(points, leaf.node, "flop_q",
                              modulePathWithLeaf(currentPath, leaf.leaf_name));
        }
        for (const auto& leaf : flopDLeafRefs(flop)) {
            addHierarchyPoint(points, leaf.node, "flop_d",
                              modulePathWithLeaf(currentPath, leaf.leaf_name));
        }
    }
    for (const auto& sub : module.hierarchyInstantiation) {
        collectHierarchyPoints(sub, currentPath + "." + sub.instance_name, points);
    }
}

std::unordered_map<const DFGNode*, std::vector<HierarchyPoint>>
buildHierarchyPointIndex(const Module& topModule) {
    std::unordered_map<const DFGNode*, std::vector<HierarchyPoint>> points;
    collectHierarchyPoints(topModule, topModule.name, points);
    return points;
}

std::vector<std::vector<const DFGNode*>> findForwardPaths(const Module& topModule,
                                                          const DFGNode* source,
                                                          const DFGNode* target) {
    if (!topModule.dfg || !source || !target) return {};

    std::unordered_map<const DFGNode*, std::vector<const DFGNode*>> consumers;
    for (const auto& node : topModule.dfg->nodes) {
        DFGTraversal::forEachInput(node.get(), [&](size_t, const DFGOutput& input) {
            consumers[input.node].push_back(node.get());
        });
    }

    std::vector<std::vector<const DFGNode*>> paths;
    std::vector<const DFGNode*> currentPath;
    std::set<const DFGNode*> active;

    std::function<void(const DFGNode*)> dfs = [&](const DFGNode* node) {
        if (!active.insert(node).second) {
            throw CompilerError(
                "debug_dfg_node_paths: encountered a cycle while enumerating paths", node);
        }

        currentPath.push_back(node);

        if (node == target) {
            paths.push_back(currentPath);
        } else if (auto it = consumers.find(node); it != consumers.end()) {
            for (const DFGNode* consumer : it->second) {
                dfs(consumer);
            }
        }

        currentPath.pop_back();
        active.erase(node);
    };

    dfs(source);
    return paths;
}

std::string relativeHierPath(const std::string& topName,
                             const std::string& fullPath) {
    const std::string prefix = topName + ".";
    if (fullPath == topName) return "";
    if (fullPath.starts_with(prefix)) return fullPath.substr(prefix.size());
    return fullPath;
}

std::vector<std::string> collapsePathToHierarchySequence(
    const std::string& topName,
    const std::vector<const DFGNode*>& path,
    const std::unordered_map<const DFGNode*, std::vector<HierarchyPoint>>& hierarchyPoints) {
    std::vector<std::string> result;
    for (const DFGNode* node : path) {
        auto it = hierarchyPoints.find(node);
        if (it == hierarchyPoints.end()) continue;
        for (const auto& point : it->second) {
            const std::string rel = relativeHierPath(topName, point.path);
            if (rel.empty()) continue;
            if (result.empty() || result.back() != rel) {
                result.push_back(rel);
            }
        }
    }
    return result;
}

void collectModuleInstanceNames(const Module& module,
                                const std::string& currentPath,
                                std::unordered_map<std::string, std::string>& names) {
    names[currentPath] = module.name;
    for (const auto& sub : module.hierarchyInstantiation) {
        const std::string subPath = currentPath.empty()
            ? sub.instance_name
            : currentPath + "." + sub.instance_name;
        collectModuleInstanceNames(sub, subPath, names);
    }
}

std::unordered_map<std::string, std::string>
buildModuleInstanceNameIndex(const Module& topModule) {
    std::unordered_map<std::string, std::string> names;
    collectModuleInstanceNames(topModule, "", names);
    return names;
}

std::string moduleContextPathForVariable(
    const std::string& relPath,
    const std::unordered_map<std::string, std::string>& moduleNames) {
    std::string best;
    size_t pos = relPath.find('.');
    while (pos != std::string::npos) {
        const std::string candidate = relPath.substr(0, pos);
        if (moduleNames.contains(candidate)) best = candidate;
        pos = relPath.find('.', pos + 1);
    }
    return best;
}

std::string moduleNameForContextPath(
    const std::string& contextPath,
    const std::unordered_map<std::string, std::string>& moduleNames) {
    if (auto it = moduleNames.find(contextPath); it != moduleNames.end()) {
        return it->second;
    }
    return contextPath;
}

std::string localNameWithinContext(const std::string& relPath,
                                   const std::string& contextPath) {
    if (contextPath.empty()) return relPath;
    const std::string prefix = contextPath + ".";
    if (relPath.starts_with(prefix)) return relPath.substr(prefix.size());
    return relPath;
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

void printRequestedNodePaths(const MateIR& ir,
                             const std::vector<DebugNodePathSpec>& specs,
                             std::ostream& out) {
    if (specs.empty()) return;

    const auto hierarchyPoints = buildHierarchyPointIndex(ir.top);
    const auto moduleNames = buildModuleInstanceNameIndex(ir.top);

    out << "  dfg_node_paths:\n";
    for (const auto& spec : specs) {
        const DFGNode* source = findGlobalSourceNode(ir.top, spec.source_name);
        if (!source) {
            throw CompilerError(std::format(
                "debug_dfg_node_paths: source '{}' not found among global INPUT nodes",
                spec.source_name));
        }

        const DebugNodeMatch match = findDebugNodeInIR(ir.top, spec.target);
        if (!match.node || !match.module) {
            throw CompilerError(std::format(
                "debug_dfg_node_paths: target '{}' not found in any module outputs, signals, or flop .d/.q leaves",
                spec.target.node_name));
        }

        const auto paths = findForwardPaths(ir.top, source, match.node);
        if (paths.empty()) {
            throw CompilerError(std::format(
                "debug_dfg_node_paths: no forward dependency path from '{}' to '{}'",
                spec.source_name, spec.target.node_name));
        }

        std::vector<std::vector<std::string>> compactPaths;
        std::set<std::string> seenCompactPaths;
        for (const auto& path : paths) {
            auto compact = collapsePathToHierarchySequence(ir.top.name, path, hierarchyPoints);
            if (compact.empty()) continue;

            std::ostringstream key;
            for (const auto& step : compact) key << step << "\n";
            if (!seenCompactPaths.insert(key.str()).second) continue;
            compactPaths.push_back(std::move(compact));
        }

        if (compactPaths.empty()) {
            throw CompilerError(std::format(
                "debug_dfg_node_paths: no hierarchy-visible path from '{}' to '{}'",
                spec.source_name, spec.target.node_name));
        }

        out << "    request: " << spec.source_name << " -> ";
        if (!spec.target.module_path.empty()) out << spec.target.module_path << ":";
        out << spec.target.node_name << "\n";
        out << "      paths: " << compactPaths.size()
            << " unique hierarchy path(s)";
        if (compactPaths.size() != paths.size()) {
            out << " (" << paths.size() << " raw DFG path(s))";
        }
        out << "\n";

        for (size_t pathIndex = 0; pathIndex < compactPaths.size(); ++pathIndex) {
            const auto& compact = compactPaths[pathIndex];
            out << "      path[" << pathIndex << "]:\n";

            size_t groupStart = 0;
            while (groupStart < compact.size()) {
                const std::string contextPath =
                    moduleContextPathForVariable(compact[groupStart], moduleNames);
                const std::string moduleName =
                    moduleNameForContextPath(contextPath, moduleNames);
                size_t groupEnd = groupStart + 1;
                while (groupEnd < compact.size() &&
                       moduleContextPathForVariable(compact[groupEnd], moduleNames) ==
                           contextPath) {
                    ++groupEnd;
                }

                out << "        (" << moduleName << ":" << contextPath << ")\n";
                out << "          ";
                for (size_t i = groupStart; i < groupEnd; ++i) {
                    if (i > groupStart) out << " -> ";
                    out << localNameWithinContext(compact[i], contextPath);
                }
                if (groupEnd < compact.size()) out << " ->";
                out << "\n";

                groupStart = groupEnd;
            }
        }
    }
}

void collectModule(const Module& module, StaticAnalysisSummary& summary) {
    summary.modules++;
    forEachInputNode(module, [&](const ModuleNode&) { summary.inputs++; });
    forEachOutputNode(module, [&](const ModuleNode&) { summary.outputs++; });
    forEachInternalNode(module, [&](const ModuleNode&) { summary.signals++; });
    summary.flops += module.flops.size();
    for (const auto& flop : module.flops) summary.flop_bits += static_cast<size_t>(flop.type.width);
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
                                               std::vector<DebugNodeSpec> debug_dfg_node_deps,
                                               std::vector<DebugNodePathSpec> debug_dfg_node_paths)
    : out_(out),
      debug_dfg_node_deps_(std::move(debug_dfg_node_deps)),
      debug_dfg_node_paths_(std::move(debug_dfg_node_paths)) {}

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
    out_ << "  flop_bits: " << summary.flop_bits << "\n";
    out_ << "  dfg_nodes: "              << summary.dfg_nodes     << "\n";
    printDomainRegistry(ir, out_);
    out_ << "  domain_usage:\n";
    printModuleDomains(ir, ir.top, out_);
    printRequestedNodeDeps(ir, debug_dfg_node_deps_, out_);
    printRequestedNodePaths(ir, debug_dfg_node_paths_, out_);
}

StaticAnalysisSummary analyzeMateIR(const MateIR& ir) {
    StaticAnalysisSummary summary;
    collectModule(ir.top, summary);
    return summary;
}

} // namespace mate
