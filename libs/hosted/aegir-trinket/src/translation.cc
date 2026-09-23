/*
 * Trinket Translation implementation.
 */

#include <aegir/trinket/translation.h>
#include <aegir/trinket/unicode.h>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace aegir::trinket {

struct Translation::Impl {
    std::unordered_map<std::string, std::string> messages_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> plural_messages_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> context_messages_;
};

Translation::Translation() = default;

Translation::~Translation() = default;

std::unique_ptr<Translation> Translation::load(std::string_view domain, std::string_view locale_dir) {
    std::string path = std::string(locale_dir) + "/translations/" + std::string(domain) + ".mo";
    return load_from_file(path);
}

std::unique_ptr<Translation> Translation::load_from_file(std::string_view path) {
    std::ifstream file(std::string(path), std::ios::binary);
    if (!file) return nullptr;

    // Read .mo header
    uint32_t magic, version, count, orig_offset, trans_offset;
    file.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != 0x950412de && magic != 0xde120495) return nullptr;  // LE/BE

    file.read(reinterpret_cast<char*>(&version), 4);
    file.read(reinterpret_cast<char*>(&count), 4);
    file.read(reinterpret_cast<char*>(&orig_offset), 4);
    file.read(reinterpret_cast<char*>(&trans_offset), 4);

    // Read string tables
    std::vector<uint32_t> orig_sizes(count), orig_offsets(count);
    std::vector<uint32_t> trans_sizes(count), trans_offsets(count);

    file.seekg(orig_offset);
    for (uint32_t i = 0; i < count; ++i) {
        file.read(reinterpret_cast<char*>(&orig_sizes[i]), 4);
        file.read(reinterpret_cast<char*>(&orig_offsets[i]), 4);
    }

    file.seekg(trans_offset);
    for (uint32_t i = 0; i < count; ++i) {
        file.read(reinterpret_cast<char*>(&trans_sizes[i]), 4);
        file.read(reinterpret_cast<char*>(&trans_offsets[i]), 4);
    }

    auto trans = std::make_unique<Translation>();

    // Read strings
    for (uint32_t i = 0; i < count; ++i) {
        // Read original
        file.seekg(orig_offsets[i]);
        std::string orig(orig_sizes[i], '\0');
        file.read(&orig[0], orig_sizes[i]);

        // Read translation
        file.seekg(trans_offsets[i]);
        std::string translation(trans_sizes[i], '\0');
        file.read(&translation[0], trans_sizes[i]);

        // Check for context (msgctxt)
        size_t eol = orig.find('\n');
        if (eol != std::string::npos) {
            std::string ctx = orig.substr(0, eol);
            std::string msgid = orig.substr(eol + 1);
            trans->impl_->context_messages_[ctx][msgid] = translation;
        } else if (orig_sizes[i] == 0) {
            // Header entry
        } else if (orig.find('\0') != std::string::npos) {
            // Plural form
            size_t null_pos = orig.find('\0');
            std::string singular = orig.substr(0, null_pos);
            std::string plural = orig.substr(null_pos + 1);
            trans->impl_->plural_messages_[singular] = {plural, translation};
        } else {
            trans->impl_->messages_[orig] = translation;
        }
    }

    return trans;
}

std::string Translation::translate(std::string_view msgid) const {
    if (!impl_) return std::string(msgid);
    auto it = impl_->messages_.find(msgid);
    if (it != impl_->messages_.end()) return it->second;
    return std::string(msgid);
}

std::string Translation::translate(std::string_view msgid, std::string_view msgctxt) const {
    if (!impl_) return std::string(msgid);
    auto ctx_it = impl_->context_messages_.find(std::string(msgctxt));
    if (ctx_it != impl_->context_messages_.end()) {
        auto it = ctx_it->second.find(msgid);
        if (it != ctx_it->second.end()) return it->second;
    }
    return std::string(msgid);
}

std::string Translation::ntranslate(std::string_view msgid, std::string_view msgid_plural, uint64_t n) const {
    if (!impl_) return n == 1 ? std::string(msgid) : std::string(msgid_plural);
    auto it = impl_->plural_messages_.find(std::string(msgid));
    if (it != impl_->plural_messages_.end()) {
        // Simplified: just return singular/plural based on n
        return n == 1 ? std::string(msgid) : std::string(msgid_plural);
    }
    return n == 1 ? std::string(msgid) : std::string(msgid_plural);
}

bool Translation::has_translation(std::string_view msgid) const {
    if (!impl_) return false;
    return impl_->messages_.find(msgid) != impl_->messages_.end();
}

static std::mutex g_translation_mutex;
static std::unique_ptr<Translation> g_global_translation;

void Translation::set_global(std::unique_ptr<Translation> t) {
    std::lock_guard<std::mutex> lock(g_translation_mutex);
    g_global_translation = std::move(t);
}

const Translation* Translation::global() {
    return g_global_translation.get();
}

} // namespace aegir::trinket