/*
 * Aegir Datatype Registry implementation.
 */

#include <aegir/datatype/registry.h>
#include <aegir/datatypes.h>
#include <algorithm>

namespace aegir::datatypes {

void Registry::register_type(std::unique_ptr<Datatype> datatype) {
    if (datatype) {
        types_[datatype->mime_type()] = std::move(datatype);
    }
}

void Registry::unregister_type(std::string_view mime_type) {
    types_.erase(std::string(mime_type));
}

Datatype* Registry::find(std::string_view mime_type) const {
    auto it = types_.find(std::string(mime_type));
    return it != types_.end() ? it->second.get() : nullptr;
}

bool Registry::load(Stream& stream, void*& object_out, std::string_view hint_mime) {
    std::string mime = std::string(hint_mime);

    // If no hint, try to detect from magic bytes
    if (mime.empty()) {
        // Save position
        int64_t pos = stream.tell();

        // Read first 8 bytes for magic detection
        uint8_t magic[8];
        size_t read;
        stream.read(magic, 8, read);
        stream.seek(pos, SEEK_SET);

        // Detect common formats
        if (read >= 8) {
            if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') {
                mime = "image/png";
            } else if (magic[0] == 0xFF && magic[1] == 0xD8) {
                mime = "image/jpeg";
            } else if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F') {
                mime = "image/gif";
            } else if (magic[0] == 'B' && magic[1] == 'M') {
                mime = "image/bmp";
            } else if (magic[0] == '%' && magic[1] == 'P' && magic[2] == 'D' && magic[3] == 'F') {
                mime = "application/pdf";
            }
        }
    }

    Datatype* dt = find(mime);
    if (!dt) return false;

    object_out = dt->create_object();
    if (!object_out) return false;

    return dt->load(stream, object_out);
}

bool Registry::save(Stream& stream, void* object, std::string_view mime_type) {
    Datatype* dt = find(mime_type);
    if (!dt) return false;
    return dt->save(stream, object);
}

std::vector<std::string> Registry::list_types() const {
    std::vector<std::string> result;
    result.reserve(types_.size());
    for (const auto& [mime, dt] : types_) {
        result.push_back(mime);
    }
    return result;
}

void register_builtin_datatypes() {
    // TODO: Register built-in datatypes (PNG, JPEG, text, etc.)
    // These would be implemented as separate Datatype subclasses
}

} // namespace aegir::datatypes