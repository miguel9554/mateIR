#include "passes/dfg_inline.h"

#include <algorithm>
#include <format>
#include <string>

namespace custom_hdl {

namespace {

void inlineModuleNode(ResolvedModule& parent, DFGNode* moduleNode) {
    const std::string& moduleTypeName = std::get<std::string>(moduleNode->data);
    const std::string& instanceName = moduleNode->name;

    // Find the sub ResolvedModule by instance name
    ResolvedModule* sub = nullptr;
    for (auto& s : parent.hierarchyInstantiation) {
        if (s.instance_name == instanceName) {
            sub = &s;
            break;
        }
    }
    if (!sub) {
        throw CompilerError(std::format(
            "inlineDFGs: MODULE node '{}' (type '{}') "
            "not found in hierarchyInstantiation",
            instanceName, moduleTypeName), moduleNode);
    }
    if (!sub->dfg) {
        throw CompilerError(std::format(
            "inlineDFGs: DFG for instance '{}' (type '{}') has already been consumed",
            instanceName, moduleTypeName), moduleNode);
    }

    // Step 1: Rewire inputs — replace uses of sub INPUT nodes with parent drivers
    for (size_t i = 0; i < moduleNode->in.size(); ++i) {
        const std::string& portName = moduleNode->input_names[i];
        DFGOutput driver = moduleNode->in[i];

        DFGNode* subInputNode = sub->dfg->getInputNode("", portName);
        if (!subInputNode) continue;

        // Replace all uses of subInputNode in sub.dfg with the parent driver
        for (auto& node : sub->dfg->nodes) {
            for (auto& inp : node->in) {
                if (inp.node == subInputNode) {
                    inp = driver;
                }
            }
        }

        // Update input binding metadata: subInputNode will be dead after
        // rewiring and removed by DCE.
        if (auto it = sub->inputs.find(portName); it != sub->inputs.end()) {
            for (auto*& leaf : it->second.binding.leaves) {
                if (leaf == subInputNode) leaf = driver.node;
            }
        }

        // Recursively fix input bindings in deeper descendants that were wired to
        // subInputNode during a prior inner inlining pass.
        std::function<void(ResolvedModule&)> fixDescendants = [&](ResolvedModule& mod) {
            for (auto& [name, inp] : mod.inputs) {
                for (auto*& leaf : inp.binding.leaves) {
                    if (leaf == subInputNode) leaf = driver.node;
                }
            }
            for (auto& child : mod.hierarchyInstantiation)
                fixDescendants(child);
        };
        for (auto& child : sub->hierarchyInstantiation)
            fixDescendants(child);
    }

    // Step 2: Rewire outputs — replace {moduleNode, portIdx} references in parent
    for (auto& node : parent.dfg->nodes) {
        for (auto& inp : node->in) {
            if (inp.node == moduleNode) {
                int portIdx = inp.port;
                if (portIdx < static_cast<int>(moduleNode->output_names.size())) {
                    const std::string& outName = moduleNode->output_names[portIdx];
                    DFGNode* subOutputNode = sub->dfg->getOutputNode("", outName);
                    if (subOutputNode) {
                        inp = DFGOutput{subOutputNode, 0};
                    }
                }
            }
        }
    }

    // Step 3: Set instance_path on all sub nodes (must happen before adopt so
    // adoptOutput/adoptInput can compute the correct map key).
    for (auto& node : sub->dfg->nodes) {
        if (node->instance_path.empty()) {
            node->instance_path = instanceName;
        } else {
            node->instance_path = instanceName + "." + node->instance_path;
        }
    }

    // Step 4: Adopt sub .d OUTPUT and .q INPUT nodes into parent's named maps.
    // Map key is computed from node->instance_path + node->name by adoptOutput/adoptInput.
    for (const auto& [name, node] : sub->dfg->getOutputsMap()) {
        if (name.ends_with(".d")) {
            parent.dfg->adoptOutput(node);
        }
    }
    for (const auto& [name, node] : sub->dfg->getInputsMap()) {
        if (name.ends_with(".q")) {
            parent.dfg->adoptInput(node);
        }
    }

    // Step 5: Move nodes from sub.dfg to parent.dfg
    for (auto& node : sub->dfg->nodes) {
        parent.dfg->nodes.push_back(std::move(node));
    }
    sub->dfg->nodes.clear();

    // Step 6: Remove MODULE node from parent.dfg
    auto& parentNodes = parent.dfg->nodes;
    parentNodes.erase(
        std::remove_if(parentNodes.begin(), parentNodes.end(),
            [moduleNode](const std::unique_ptr<DFGNode>& n) {
                return n.get() == moduleNode;
            }),
        parentNodes.end());

    // Step 7: Null sub DFG — binding pointers remain valid since the DFGNode
    // objects now live in parent.dfg->nodes.
    sub->dfg.reset();
}

} // anonymous namespace

void inlineDFGs(ResolvedModule& top) {
    // Bottom-up: recurse into children first so each sub DFG is flat before we inline it
    for (auto& sub : top.hierarchyInstantiation) {
        inlineDFGs(sub);
    }

    if (!top.dfg) return;

    // Collect all MODULE nodes before modifying the nodes vector
    std::vector<DFGNode*> moduleNodes;
    for (const auto& node : top.dfg->nodes) {
        if (node->op == DFGOp::MODULE) {
            moduleNodes.push_back(node.get());
        }
    }

    for (DFGNode* moduleNode : moduleNodes) {
        inlineModuleNode(top, moduleNode);
    }
}

} // namespace custom_hdl
