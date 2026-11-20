// TITAN Engine — editor transactions and undo/redo. Original TITAN code.
#include "titan/edit.h"

#include <algorithm>
#include <set>

namespace titan {

namespace {
template <class Vec, class Id>
auto findChange(Vec& v, const Id& id) {
    return std::find_if(v.begin(), v.end(), [&](const auto& c) { return c.id == id; });
}
}  // namespace

void SceneEdit::recordUpdate(const Scene& scene, const SceneObject& after) {
    auto it = findChange(changes_, after.id);
    if (it != changes_.end()) {
        it->after = after;
        return;
    }
    const SceneObject* before = scene.find(after.id);
    changes_.push_back({after.id, before ? std::optional<SceneObject>(*before) : std::nullopt, after});
}

void SceneEdit::recordCreate(const SceneObject& obj) {
    auto it = findChange(changes_, obj.id);
    if (it != changes_.end()) {
        it->after = obj;
        return;
    }
    changes_.push_back({obj.id, std::nullopt, obj});
}

void SceneEdit::recordDelete(const Scene& scene, ObjectId id) {
    auto it = findChange(changes_, id);
    if (it != changes_.end()) {
        it->after.reset();
        return;
    }
    const SceneObject* before = scene.find(id);
    if (!before) return;
    changes_.push_back({id, *before, std::nullopt});
}

void SceneEdit::recordRecipe(const Scene& scene, std::optional<Recipe> after, const std::string& id) {
    auto it = findChange(recipeChanges_, id);
    if (it != recipeChanges_.end()) {
        it->after = std::move(after);
        return;
    }
    const Recipe* before = scene.findRecipe(id);
    recipeChanges_.push_back({id, before ? std::optional<Recipe>(*before) : std::nullopt, std::move(after)});
}

void SceneEdit::apply(Scene& scene) const {
    // Creates/updates in recorded order (parents are recorded before children by callers),
    // deletes afterwards in reverse so children go first.
    for (const auto& c : changes_)
        if (c.after) scene.put(*c.after);
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it)
        if (!it->after) scene.erase(it->id);
    for (const auto& r : recipeChanges_) {
        if (r.after) scene.putRecipe(*r.after);
        else scene.eraseRecipe(r.id);
    }
    scene.bumpRevision();
}

void SceneEdit::revert(Scene& scene) const {
    for (const auto& c : changes_)
        if (c.before) scene.put(*c.before);
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it)
        if (!it->before) scene.erase(it->id);
    for (const auto& r : recipeChanges_) {
        if (r.before) scene.putRecipe(*r.before);
        else scene.eraseRecipe(r.id);
    }
    scene.bumpRevision();
}

bool SceneEdit::tryMerge(const SceneEdit& later) {
    if (coalesceKey_.empty() || coalesceKey_ != later.coalesceKey_) return false;
    if (!recipeChanges_.empty() || !later.recipeChanges_.empty()) return false;
    for (const auto& lc : later.changes_) {
        auto it = findChange(changes_, lc.id);
        if (it == changes_.end()) changes_.push_back(lc);  // keep later's before (state at that time)
        else it->after = lc.after;                           // keep our before, take later's after
    }
    return true;
}

bool SceneEdit::empty() const {
    for (const auto& c : changes_)
        if (c.before != c.after) return false;
    for (const auto& r : recipeChanges_)
        if (r.before.has_value() != r.after.has_value() ||
            (r.before && (r.before->params != r.after->params || r.before->extension != r.after->extension)))
            return false;
    return true;
}

std::vector<ObjectId> SceneEdit::touchedIds() const {
    std::vector<ObjectId> ids;
    for (const auto& c : changes_) ids.push_back(c.id);
    return ids;
}

void UndoStack::push(Scene& scene, SceneEdit edit) {
    edit.apply(scene);
    const bool redoDiscarded = !undone_.empty();
    if (redoDiscarded) {
        // The saved state may have lived on the redo branch; it is now unreachable.
        if (savedDepth_ && *savedDepth_ > done_.size()) savedDepth_.reset();
        undone_.clear();
    }
    // coalesceOpen_ is cleared by undo/redo/save, so merging never crosses those boundaries.
    if (coalesceOpen_ && !redoDiscarded && !done_.empty() && done_.back().tryMerge(edit)) return;
    done_.push_back(std::move(edit));
    coalesceOpen_ = !done_.back().coalesceKey().empty();
    if (done_.size() > limit_) {
        done_.erase(done_.begin());
        if (savedDepth_) {
            if (*savedDepth_ == 0) savedDepth_.reset();
            else --*savedDepth_;
        }
    }
}

std::vector<std::string> UndoStack::labels() const {
    std::vector<std::string> out;
    out.reserve(size());
    for (const auto& e : done_) out.push_back(e.label());
    for (auto it = undone_.rbegin(); it != undone_.rend(); ++it) out.push_back(it->label());  // next redo first
    return out;
}

std::optional<std::string> UndoStack::undoLabel() const {
    if (done_.empty()) return std::nullopt;
    return done_.back().label();
}

std::optional<std::string> UndoStack::redoLabel() const {
    if (undone_.empty()) return std::nullopt;
    return undone_.back().label();
}

std::optional<std::vector<ObjectId>> UndoStack::undo(Scene& scene) {
    if (done_.empty()) return std::nullopt;
    coalesceOpen_ = false;
    SceneEdit e = std::move(done_.back());
    done_.pop_back();
    e.revert(scene);
    auto ids = e.touchedIds();
    undone_.push_back(std::move(e));
    return ids;
}

std::optional<std::vector<ObjectId>> UndoStack::redo(Scene& scene) {
    if (undone_.empty()) return std::nullopt;
    coalesceOpen_ = false;
    SceneEdit e = std::move(undone_.back());
    undone_.pop_back();
    e.apply(scene);
    auto ids = e.touchedIds();
    done_.push_back(std::move(e));
    return ids;
}

void UndoStack::markSaved() {
    savedDepth_ = done_.size();
    coalesceOpen_ = false;  // never merge across a save point
}

bool UndoStack::isDirty() const { return !savedDepth_ || *savedDepth_ != done_.size(); }

void UndoStack::clear() {
    done_.clear();
    undone_.clear();
    savedDepth_ = 0;
    coalesceOpen_ = false;
}

}  // namespace titan
