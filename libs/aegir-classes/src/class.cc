/*
 * Aegir Classes implementation.
 */

#include <aegir/classes.h>
#include <algorithm>

namespace aegir::classes {

Object::Object() = default;
Object::~Object() = default;

uintptr_t Object::dispatch(Message* msg) {
    // Default: pass to superclass or return 0
    return 0;
}

void Class::register_class(std::unique_ptr<Class> cls) {
    instance().classes_[cls->name_] = std::move(cls);
}

void Class::unregister_class(std::string_view name) {
    instance().classes_.erase(std::string(name));
}

Class* Class::find_class(std::string_view name) const {
    auto it = instance().classes_.find(std::string(name));
    return it != instance().classes_.end() ? it->second.get() : nullptr;
}

Object* Class::create_object(std::string_view class_name, TagItem* tags) {
    Class* cls = find_class(class_name);
    return cls ? cls->create(tags) : nullptr;
}

} // namespace aegir::classes