/*
 * Aegir Classes - BOOPSI-inspired class system.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_CLASSES_H
#define AEGIR_CLASSES_H

#include <cstdint>
#include <memory>
#include <string>

namespace aegir::classes {

// Forward declaration
class Object;
class Message;

// Method IDs (like Amiga BOOPSI)
enum class MethodID : uint32_t {
    OM_NEW = 1,           // Create object
    OM_DISPOSE = 2,       // Destroy object
    OM_SET = 3,           // Set attributes
    OM_GET = 4,           // Get attributes
    OM_UPDATE = 5,        // Update attributes
    OM_NOTIFY = 6,        // Notification
    OM_ADDTAIL = 7,       // Add to list tail
    OM_REMOVE = 8,        // Remove from list
    OM_INSERT = 9,        // Insert into list
    OM_RENDER = 10,       // Render
    OM_LAYOUT = 11,       // Layout
    OM_HITTEST = 12,      // Hit test
    OM_GOACTIVE = 13,     // Go active (input focus)
    OM_HANDLEINPUT = 14,  // Handle input
    OM_GOINACTIVE = 15,   // Go inactive

    // Custom methods start at 0x1000
    CUSTOM_BASE = 0x1000
};

// Attribute tags (like Amiga taglists)
struct TagItem {
    uint32_t tag;      // Attribute ID
    uintptr_t data;    // Value (or pointer)
};

// Base message structure
struct Message {
    MethodID method_id;
    // Method-specific data follows
};

// New message
struct NewMessage : Message {
    TagItem* tags;
};

// Set message
struct SetMessage : Message {
    TagItem* tags;
};

// Get message
struct GetMessage : Message {
    TagItem* tags;
};

// Dispose message
struct DisposeMessage : Message {};

// Update message
struct UpdateMessage : Message {
    TagItem* tags;
};

// Notify message
struct NotifyMessage : Message {
    TagItem* tags;
};

// Base object class
class Object {
public:
    Object() = default;
    virtual ~Object() = default;

    // Dispatcher - called by class system
    virtual uintptr_t dispatch(Message* msg) = 0;

    // Helper methods
    template<typename T>
    T get_attr(uint32_t tag) const {
        TagItem tags[2] = {{tag, 0}, {0, 0}};
        GetMessage msg{MethodID::OM_GET, tags};
        const_cast<Object*>(this)->dispatch(reinterpret_cast<Message*>(&msg));
        return static_cast<T>(tags[0].data);
    }

    template<typename T>
    void set_attr(uint32_t tag, T value) {
        TagItem tags[2] = {{tag, reinterpret_cast<uintptr_t>(value)}, {0, 0}};
        SetMessage msg{MethodID::OM_SET, tags};
        dispatch(reinterpret_cast<Message*>(&msg));
    }
};

// Class descriptor
class Class {
public:
    using Dispatcher = uintptr_t(*)(Object*, Message*);

    Class(std::string_view name, Class* super = nullptr, Dispatcher dispatch = nullptr)
        : name_(name), super_(super), dispatch_(dispatch) {}

    virtual ~Class() = default;

    const std::string& name() const { return name_; }
    Class* super() const { return super_; }
    Dispatcher dispatcher() const { return dispatch_; }

    // Create instance
    Object* create(TagItem* tags = nullptr) const {
        // Allocate object
        Object* obj = new Object();
        // Call OM_NEW
        NewMessage msg{MethodID::OM_NEW, tags};
        obj->dispatch(reinterpret_cast<Message*>(&msg));
        return obj;
    }

protected:
    std::string name_;
    Class* super_ = nullptr;
    Dispatcher dispatch_ = nullptr;
};

// Class registry
class Registry {
public:
    static Registry& instance() {
        static Registry instance;
        return instance;
    }

    // Register a class
    void register_class(std::unique_ptr<Class> cls);

    // Unregister by name
    void unregister_class(std::string_view name);

    // Find class by name
    Class* find_class(std::string_view name) const;

    // Create object by class name
    Object* create_object(std::string_view class_name, TagItem* tags = nullptr);

private:
    Registry() = default;
    std::unordered_map<std::string, std::unique_ptr<Class>> classes_;
};

// Standard tags
enum class Tag : uint32_t {
    DONE = 0,
    IGNORE = 1,

    // Visual
    LEFT = 0x8000,
    TOP = 0x8001,
    WIDTH = 0x8002,
    HEIGHT = 0x8003,
    VISIBLE = 0x8004,
    ENABLED = 0x8005,

    // Text
    TEXT = 0x8010,
    FONT = 0x8011,
    ALIGNMENT = 0x8012,

    // Colors
    BG_COLOR = 0x8020,
    FG_COLOR = 0x8021,
    BORDER_COLOR = 0x8022,

    // Callbacks
    ON_CLICK = 0x8030,
    ON_CHANGE = 0x8031,
    ON_FOCUS = 0x8032,

    // Custom tags start at 0x10000
    CUSTOM_BASE = 0x10000
};

} // namespace aegir::classes

#endif // AEGIR_CLASSES_H