// TITAN Engine — editor/engine capability contract. Original TITAN code.
//
// One registry, shared by every engine backend (reference host, Unreal bridge) and by TITAN Studio:
// each capability has a version and the protocol methods it covers. A backend advertises the capabilities it
// really implements in session.hello ("capabilities": [names], "capabilityVersions": {name: version}); a
// request for a method of a capability the backend does not advertise is answered with kUnsupported (never
// silently ignored or simulated), and Studio refuses it locally without a round trip. Methods not listed here
// (session.*, project.*, scene.*, object.get/create/update/delete, selection.*, edit.undo/redo,
// engine.*, editor.*) are part of the base protocol every backend must implement.
// Bump a capability's version when its methods or results change incompatibly.
#pragma once

#include <string>
#include <vector>

namespace titan {

struct CapabilitySpec {
    const char* name;
    int version;
    std::vector<const char*> methods;  // protocol methods that require this capability
};

const std::vector<CapabilitySpec>& capabilityRegistry();
// The capability a method belongs to, or nullptr for base-protocol methods.
const CapabilitySpec* capabilityForMethod(const std::string& method);
const CapabilitySpec* findCapability(const std::string& name);

}  // namespace titan
