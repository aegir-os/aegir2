/*
 * Aegir Datatype Registry.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#ifndef AEGIR_DATATYPE_REGISTRY_H
#define AEGIR_DATATYPE_REGISTRY_H

#include <aegir/datatypes.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace aegir::datatypes {

class Registry {
public:
    static Registry& instance() {
        static Registry instance;
        return instance;
    }

    // Register a datatype handler
    // Takes ownership of the datatype
    void register_type(std::unique_ptr<Datatype> datatype);

    // Unregister by MIME type
    void unregister_type(std::string_view mime_type);

    // Find datatype by MIME type
    Datatype* find(std::string_view mime_type) const;

    // Load object from stream, auto-detecting type
    // For auto-detection, stream must support seeking
    bool load(Stream& stream, void*& object_out, std::string_view hint_mime = "");

    // Save object to stream
    bool save(Stream& stream, void* object, std::string_view mime_type);

    // List all registered MIME types
    std::vector<std::string> list_types() const;

private:
    Registry() = default;
    std::unordered_map<std::string, std::unique_ptr<Datatype>> types_;
};

// Built-in datatypes (registered automatically)
void register_builtin_datatypes();

} // namespace aegir::datatypes

#endif // AEGIR_DATATYPE_REGISTRY_H