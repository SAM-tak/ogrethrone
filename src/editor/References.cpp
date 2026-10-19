#include "editor/References.h"

namespace throne {
References findReferences(const LevelExpansion& view, const Level* level) {
    References result;
    // Resolves @p ref like ScriptRuntime does: relative to the system's instance
    // (@p base in view instance @p from), or from the scene instance it names.
    const auto resolve = [&](std::size_t from, const NodePath& base, const NodeRef& ref, const Referrer& referrer) {
        // A level system (from no instance) reaches nodes only through a scene instance.
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
    const auto scan = [&](const System& system, int instance, std::size_t from, const NodePath& base) {
        for (std::size_t k = 0; k < system.behaviour.size(); ++k)
            for (const auto& [name, value] : system.behaviour[k].data)
                if (const auto* ref = std::get_if<NodeRef>(&value))
                    resolve(from, base, *ref,
                            {instance, base, system.id, k, name,
                             system.name + " > " + system.behaviour[k].script + " > " + name});
    };
    for (std::size_t i = 0; i < view.instances.size(); ++i) {
        for (const auto& expanded : view.instances[i].expansion.systems)
            scan(expanded.system, static_cast<int>(i), i, expanded.instance);
        for (const auto& expanded : view.instances[i].expansion.nodes) {
            auto base = expanded.path;
            if (!base.empty() && !expanded.root) base.pop_back();
            const auto fields = [&](const Behaviour& attachment, std::size_t k) {
                for (const auto& [name, value] : attachment.data)
                    if (const auto* ref = std::get_if<NodeRef>(&value))
                        resolve(i, base, *ref, {static_cast<int>(i), expanded.path, 0, k, name,
                            expanded.node.name + " > " + attachment.script + " > " + name});
            };
            for (std::size_t k = 0; k < expanded.node.behaviour.size(); ++k) fields(expanded.node.behaviour[k], k);
            for (const auto& [name, component] : expanded.node.scriptComponents) fields(component, 0);
        }
    }
    // A level system names the scene instance of every node it refers to.
    if (level)
        for (const auto& system : level->systems) scan(system, -1, view.instances.size(), {});
    return result;
}
}
