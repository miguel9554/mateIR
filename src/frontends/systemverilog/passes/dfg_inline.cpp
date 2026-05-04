#include "frontends/systemverilog/passes/dfg_inline.h"

#include <algorithm>
#include <format>
#include <functional>
#include <set>
#include <string>

namespace mate {

namespace {

void inlineModuleNode(Module& parent, ModuleInstanceBinding& binding) {
    const std::string& moduleTypeName = binding.module_type;
    const std::string& instanceName = binding.instance_name;

    auto it = std::find_if(parent.hierarchyInstantiation.begin(),
                           parent.hierarchyInstantiation.end(),
                           [&](auto& s) { return s.instance_name == instanceName; });
    if (it == parent.hierarchyInstantiation.end()) {
        throw CompilerError(std::format(
            "inlineDFGs: instance binding '{}' (type '{}') not found in hierarchyInstantiation",
            instanceName, moduleTypeName));
    }
    Module* sub = &(*it);
    if (!sub->dfg) {
        throw CompilerError(std::format(
            "inlineDFGs: DFG for instance '{}' (type '{}') has already been consumed",
            instanceName, moduleTypeName));
    }

    // Step 1: Rewire inputs — replace uses of sub INPUT nodes with parent drivers
    for (const auto& inputBinding : binding.inputs) {
        const std::string& portName = inputBinding.port_name;
        DFGOutput driver = inputBinding.driver;

        DFGNode* subInputNode = sub->dfg->getGraphInput("", portName);
        if (!subInputNode) continue;

        // Replace all uses of subInputNode in sub.dfg with the parent driver
        for (auto& node : sub->dfg->nodes) {
            node->replaceInputNode(subInputNode, driver);
        }

        // Update input binding metadata: subInputNode will be dead after
        // rewiring and removed by DCE.
        if (auto* input = findInputNode(*sub, portName); input) {
            for (auto*& leaf : input->binding.leaves) {
                if (leaf == subInputNode) leaf = driver.node;
            }
            for (auto& leaf : input->binding.aggregate_leaves) {
                if (leaf.leaf == subInputNode) leaf.leaf = driver.node;
            }
        }

        // Recursively fix input bindings in deeper descendants that were wired to
        // subInputNode during a prior inner inlining pass.
        std::function<void(Module&)> fixDescendants = [&](Module& mod) {
            forEachInputNode(mod, [&](ModuleNode& inp) {
                for (auto*& leaf : inp.binding.leaves) {
                    if (leaf == subInputNode) leaf = driver.node;
                }
                for (auto& leaf : inp.binding.aggregate_leaves) {
                    if (leaf.leaf == subInputNode) leaf.leaf = driver.node;
                }
            });
            for (auto& child : mod.hierarchyInstantiation)
                fixDescendants(child);
        };
        for (auto& child : sub->hierarchyInstantiation)
            fixDescendants(child);
    }

    // Step 2: Rewire parent-side child output placeholders to actual child outputs.
    for (const auto& [portName, placeholder] : binding.output_placeholders) {
        DFGNode* subOutputNode = sub->dfg->getGraphOutput("", portName);
        if (!subOutputNode) continue;
        parent.dfg->redirectConsumers(placeholder, DFGOutput{subOutputNode, 0});
    }

    // Step 3: Set instance_path on all sub nodes (must happen before adopt so
    // adoptGraphOutput/adoptGraphInput can compute the correct map key).
    for (auto& node : sub->dfg->nodes) {
        if (node->instance_path.empty()) {
            node->instance_path = instanceName;
        } else {
            node->instance_path = instanceName + "." + node->instance_path;
        }
    }

    // Step 4: Adopt sub .d GraphOutput and .q GraphInput nodes into the parent boundary maps.
    // Map key is computed from node->instance_path + node->name by adoptGraphOutput/adoptGraphInput.
    sub->dfg->forEachGraphOutput([&](const std::string&, DFGNode* node) {
        if (node->name.ends_with(".d")) {
            parent.dfg->adoptGraphOutput(node);
        }
    });
    sub->dfg->forEachGraphInput([&](const std::string&, DFGNode* node) {
        if (node->name.ends_with(".q")) {
            parent.dfg->adoptGraphInput(node);
        }
    });

    // Step 5: Move nodes from sub.dfg to parent.dfg
    for (auto& node : sub->dfg->nodes) {
        parent.dfg->nodes.push_back(std::move(node));
    }
    sub->dfg->nodes.clear();

    // Step 6: Null sub DFG — binding pointers remain valid since the DFGNode
    // objects now live in parent.dfg->nodes.
    sub->dfg.reset();
}

} // anonymous namespace

void inlineDFGs(Module& top) {
    // Bottom-up: recurse into children first so each sub DFG is flat before we inline it
    for (auto& sub : top.hierarchyInstantiation) {
        inlineDFGs(sub);
    }

    if (!top.dfg) return;

    std::set<std::string> seenInstanceNames;
    for (const auto& child : top.hierarchyInstantiation) {
        if (!seenInstanceNames.insert(child.instance_name).second) {
            throw CompilerError(std::format(
                "inlineDFGs: duplicate child instance_name '{}' in module '{}'",
                child.instance_name, top.name));
        }
    }
    for (const auto& binding : top.instance_bindings) {
        if (!seenInstanceNames.contains(binding.instance_name)) {
            throw CompilerError(std::format(
                "inlineDFGs: instance binding '{}' has no matching child module in '{}'",
                binding.instance_name, top.name));
        }
    }

    for (auto& binding : top.instance_bindings) {
        inlineModuleNode(top, binding);
    }
    top.instance_bindings.clear();
    rebuildModuleNodeIndexRecursively(top);
}

} // namespace mate
