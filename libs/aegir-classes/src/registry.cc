/*
 * Aegir Class Registry implementation.
 */

#include <aegir/classes.h>

namespace aegir::classes {

void Registry::register_class(std::unique_ptr<Class> cls) {
    if (cls) {
        classes_[cls->name()] = std::move(cls);
    }
}

void Registry::unregister_class(std::string_view name) {
    classes_.erase(std::string(name));
}

Class* Registry::find_class(std::string_view name) const {
    auto it = classes_.find(std::string(name));
    return it != classes_.end() ? it->second.get() : nullptr;
}

Object* Registry::create_object(std::string_view class_name, TagItem* tags) {
    Class* cls = find_class(class_name);
    return cls ? cls->create(tags) : nullptr;
}

} // namespace aegir::classes