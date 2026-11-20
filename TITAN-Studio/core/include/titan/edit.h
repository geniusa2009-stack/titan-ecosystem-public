// TITAN Engine — editor services: reversible scene edits, transactions and undo/redo.
// Every scene mutation requested by any editor (Qt Studio, the Slate bridge, AI tools,
// extensions) is expressed as a SceneEdit so it is undoable and auditable.
// Original TITAN code.
#pragma once

#include "titan/scene.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace titan {

// One reversible change: a set of object snapshots before/after (nullopt = absent),
// plus optional recipe snapshots for World Builder provenance.
class SceneEdit {
public:
    struct ObjectChange {
        ObjectId id = kNoObject;
        std::optional<SceneObject> before;
        std::optional<SceneObject> after;
    };
    struct RecipeChange {
        std::string id;
        std::optional<Recipe> before;
        std::optional<Recipe> after;
    };

    explicit SceneEdit(std::string label, std::string coalesceKey = {})
        : label_(std::move(label)), coalesceKey_(std::move(coalesceKey)) {}

    // Builders capture "before" from the live scene at the time of the call.
    void recordUpdate(const Scene& scene, const SceneObject& after);
    void recordCreate(const SceneObject& obj);
    void recordDelete(const Scene& scene, ObjectId id);
    void recordRecipe(const Scene& scene, std::optional<Recipe> after, const std::string& id);

    void apply(Scene& scene) const;
    void revert(Scene& scene) const;

    // Merge a later edit into this one (e.g. continuous drag). Returns false if incompatible.
    bool tryMerge(const SceneEdit& later);

    bool empty() const;
    const std::string& label() const { return label_; }
    const std::string& coalesceKey() const { return coalesceKey_; }
    const std::vector<ObjectChange>& changes() const { return changes_; }
    std::vector<ObjectId> touchedIds() const;

private:
    std::string label_;
    std::string coalesceKey_;
    std::vector<ObjectChange> changes_;
    std::vector<RecipeChange> recipeChanges_;
};

class UndoStack {
public:
    explicit UndoStack(size_t limit = 512) : limit_(limit) {}

    // Applies the edit to the scene and records it. Coalesces with the previous edit when
    // both carry the same non-empty coalesce key and nothing was undone in between.
    void push(Scene& scene, SceneEdit edit);

    bool canUndo() const { return !done_.empty(); }
    bool canRedo() const { return !undone_.empty(); }
    std::optional<std::string> undoLabel() const;
    std::optional<std::string> redoLabel() const;

    // Return the touched ids of the edit that was reverted/re-applied.
    std::optional<std::vector<ObjectId>> undo(Scene& scene);
    std::optional<std::vector<ObjectId>> redo(Scene& scene);

    // Labels of the whole history, oldest first: the first depth() entries are done, the rest can be redone
    // (next redo first). savedPosition() is the history position of the last save, or nullopt if unreachable.
    std::vector<std::string> labels() const;
    std::optional<size_t> savedPosition() const { return savedDepth_; }
    size_t size() const { return done_.size() + undone_.size(); }

    void breakCoalescing() { coalesceOpen_ = false; }
    void markSaved();
    bool isDirty() const;
    void clear();
    size_t depth() const { return done_.size(); }

private:
    std::vector<SceneEdit> done_;
    std::vector<SceneEdit> undone_;
    size_t limit_;
    // Number of edits in done_ at the last save, or nullopt if that state is unreachable.
    std::optional<size_t> savedDepth_ = 0;
    bool coalesceOpen_ = false;
};

}  // namespace titan
