// TITAN Engine — capability registry (see titan/capabilities.h). Original TITAN code.
#include "titan/capabilities.h"

namespace titan {

const std::vector<CapabilitySpec>& capabilityRegistry() {
    static const std::vector<CapabilitySpec> r = {
        // Advertised by every backend today; versioned so future changes can be negotiated.
        {"project", 1, {}},
        {"scene", 1, {}},
        {"selection", 1, {}},
        {"undo", 1, {}},
        {"pick", 1, {"scene.pick"}},
        // Optional features: a backend advertises only what it implements.
        {"extensions", 1, {"extensions.invoke"}},
        {"extensions.manage", 1, {"extensions.setEnabled", "extensions.reload"}},
        {"worldbuilder.preview", 1, {"worldbuilder.preview", "worldbuilder.approve", "worldbuilder.reject"}},
        {"assets", 1, {"assets.list", "assets.refresh"}},
        {"prefabs", 1, {"prefab.create", "prefab.instantiate"}},
        {"edit.duplicate", 1, {"object.duplicate"}},
        {"edit.group", 1, {"object.group", "object.ungroup"}},
        {"edit.multi", 1, {"object.updateMany", "object.transformMany"}},
        {"edit.reparent", 1, {"object.reparent"}},
        {"edit.lock", 1, {}},
        {"camera.bookmarks", 1, {}},
        {"edit.clipboard", 1, {"object.copy", "object.paste"}},
        {"edit.history", 1, {"edit.history", "edit.goto"}},  // editor.setState {bookmarks}, returned in project.open "editor"  // object property "locked" (object.update / object.updateMany)
        {"recovery", 1, {"project.autosave", "project.recover", "project.discardRecovery"}},
        {"templates", 1, {"templates.list", "project.saveTemplate"}},
        {"viewport.frames", 1, {"viewport.configure", "viewport.camera", "viewport.render"}},
        {"unreal.levels", 1, {}},
    };
    return r;
}

const CapabilitySpec* capabilityForMethod(const std::string& method) {
    for (const auto& c : capabilityRegistry())
        for (const char* m : c.methods)
            if (method == m) return &c;
    return nullptr;
}

const CapabilitySpec* findCapability(const std::string& name) {
    for (const auto& c : capabilityRegistry())
        if (name == c.name) return &c;
    return nullptr;
}

}  // namespace titan
