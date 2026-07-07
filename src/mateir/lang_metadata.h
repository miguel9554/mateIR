#pragma once

// Language-metadata sidecar.
//
// Frontends lower source-language constructs (e.g. SystemVerilog interfaces)
// into plain MateIR entities. This sidecar records the mapping that the
// lowering destroys, and nothing else:
//
//   Derivability rule: a fact goes into the sidecar only if it CANNOT be
//   recomputed from the IR. Types, directions, member lists, and parameter
//   values live on the anchored IR entities and must never be duplicated
//   here. The IR remains the single source of truth.
//
//   One-way flow: the frontend writes records during elaboration; consumers
//   (debug artifacts, hierarchy tooling, DPI codegen) read them. Passes never
//   see the sidecar — it is attached to the MateIR container only after the
//   pass pipeline has completed, so no pass has an access path to it.
//
// The record core is language-agnostic. Everything language-specific lives in
// the `kind` string and its documented attrs/role schema:
//
//   kind "sv.interface_port" — an interface-typed module port.
//     name:  the port name (e.g. "bus")
//     attrs: interface = source interface type name
//            modport   = declaration-site modport name
//     bindings: role "member:<member>" -> module_node "<port>.<member>"
//               role "param:<param>"   -> param       "<port>.<param>"
//
//   kind "sv.interface_instance" — an interface instance inside a module.
//     name:  the instance name (e.g. "b12")
//     attrs: interface = source interface type name
//     bindings: role "member:<member>" -> module_node "<inst>.<member>"
//               role "param:<param>"   -> param       "<inst>.<param>"
//
//   kind "sv.port_type" — a top-level port declared with an externally
//     referenceable named type (package-scoped in source, or a bare typedef
//     name resolved through package imports).
//     name:  the port name (e.g. "in_s")
//     attrs: type = qualified source type name (e.g. "pkg::payload_t")
//     bindings: exactly one, role "port" -> module_node "<port>" in the top

#include <map>
#include <set>
#include <string>
#include <vector>

namespace mate {

struct Module;

// The address of an IR entity: always canonical names, never pointers.
struct LangMetaAnchor {
    // Dotted instance path of the owning module from the top ("" = top).
    std::string module_path;
    // "module_node" | "param"
    std::string entity;
    // Canonical entity name within the module (e.g. "bus.data").
    std::string name;
};

struct LangMetaBinding {
    std::string role;
    LangMetaAnchor anchor;
};

struct LangMetaRecord {
    std::string kind;  // dialect-tagged, e.g. "sv.interface_port"
    std::string name;  // source-level name of the construct instance
    std::map<std::string, std::string> attrs;
    std::vector<LangMetaBinding> bindings;
};

struct LangMetadata {
    std::vector<LangMetaRecord> records;
};

std::string langMetadataToJson(const LangMetadata& meta, int indent = 0);

// Verify every record against its kind's schema and resolve every anchor
// against the module hierarchy. Throws CompilerError on any violation.
void validateLangMetadata(const LangMetadata& meta, const Module& top);

// ============================================================================
// Named-entity invariant
// ============================================================================
// Sidecar anchors stay valid because named entities in a Module (nodes,
// flops, parameters, localparams) are created by elaboration and never
// renamed nor deleted by any pass. Snapshot the entity names after
// elaboration and assert the set is unchanged after the pipeline.
//
// One sanctioned canonicalization exists: flop_resolve expands each aggregate
// FlopInfo into per-leaf FlopInfos whose names are the deterministic leaf
// plan (collectAggregateLeafPlan) of the original name and type. Flops are
// therefore snapshotted by leaf name, which is stable across that expansion.

struct NamedEntitySnapshot {
    // module instance path ("" = top) -> "<entity-kind>:<name>" entries
    std::map<std::string, std::set<std::string>> by_module_path;
};

NamedEntitySnapshot collectNamedEntities(const Module& top);

// Throws CompilerError listing any renamed/deleted named entity.
void assertNamedEntitiesUnchanged(const NamedEntitySnapshot& before,
                                  const Module& top);

} // namespace mate
