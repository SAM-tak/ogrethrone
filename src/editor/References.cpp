#include "editor/References.h"

namespace throne {
References findReferences(const LevelExpansion& view, const Level* level) {
    References result;
    // Resolves @p ref like ScriptRuntime does: relative to the runner's instance
    // (@p base in view instance @p from), or from the scene instance it names.
    const auto resolve = [&](std::size_t from, const NodePath& base, const NodeRef& ref, const Referrer& referrer) {
        // A level runner (from no instance) reaches nodes only through a scene instance.
        if (!ref.node || (!ref.scene && from >= view.instances.size())) return;
        auto target = from;
        auto path = base;
        if (ref.scene) {
            target = view.instances.size();
            for (std::size_t i = 0; i < view.instances.size(); ++i)
                if (view.instances[i].id == ref.scene) target = i;
            if (target == view.instances.size()) return;
            path.clear();
        }
        path.push_back(ref.node);
        path.insert(path.end(), ref.inner.begin(), ref.inner.end());
        if (const auto at = view.instances[target].expansion.find(path); at != Expansion::npos)
            result[{target, at}].push_back(referrer);
    };
    const auto scan = [&](const Runner& runner, int instance, std::size_t from, const NodePath& base) {
        for (std::size_t k = 0; k < runner.behaviours.size(); ++k)
            for (const auto& [name, value] : runner.behaviours[k].fields)
                if (const auto* ref = std::get_if<NodeRef>(&value))
                    resolve(from, base, *ref,
                            {instance, base, runner.id, k, name,
                             runner.name + " > " + runner.behaviours[k].script + " > " + name});
    };
    for (std::size_t i = 0; i < view.instances.size(); ++i)
        for (const auto& expanded : view.instances[i].expansion.runners)
            scan(expanded.runner, static_cast<int>(i), i, expanded.instance);
    // A level runner names the scene instance of every node it refers to.
    if (level)
        for (const auto& runner : level->runners) scan(runner, -1, view.instances.size(), {});
    return result;
}
}
