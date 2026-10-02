/*
 * Trinket FileRequester: the Amiga's Open requester.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A `Requester` whose body is a file list: a titles row and the drawer's
 * entries (name, size, date, time) with drawer icons, over a `Pattern` box, a
 * `Drawer` toggle and path, and a `File` box. The list is filtered by an
 * AmigaDOS wildcard (specs/pattern.md), the `Pattern` box's own; the `Volumes`
 * action lists the browsable volumes -- those without `kFlagNoDir`
 * (specs/vfs.md). The VFS is the *client's*: the requester lists the drawer it
 * is given, so the program that opens a file is the one whose manifest holds
 * `vfs.namespace` (specs/trinket/file_requester.md).
 */

#ifndef AEGIR_TRINKET_FILE_REQUESTER_H
#define AEGIR_TRINKET_FILE_REQUESTER_H

#include <aegir/trinket/application.h>
#include <aegir/trinket/requester.h>
#include <aegir/vfs.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class Button;
class ListView;
class Scrollbar;
class TextBox;

class FileRequester {
public:
    /** The bottom row's ids, as the Requester's `Action`s carry them. */
    enum Action { OK = 1, VOLUMES = 2, PARENT = 3, CANCEL = 4 };

    FileRequester(Application& app, aegir::vfs::Namespace& vfs, std::u32string_view title);
    FileRequester(Application& app, aegir::vfs::Namespace& vfs, std::string_view title);
    ~FileRequester();

    FileRequester(FileRequester const&) = delete;
    FileRequester& operator=(FileRequester const&) = delete;

    /** The drawer the list opens on. It is listed at once, so a drawer that
     *  does not resolve leaves an empty list. */
    void open_at(std::u32string_view drawer);

    /** The wildcard the list is filtered by, `#?` (everything) at first. */
    void set_pattern(std::u32string_view pattern);

    std::u32string const& drawer() const { return drawer_; }
    std::u32string const& pattern() const { return pattern_; }
    /** The name in the File box when OK resolved, empty otherwise. */
    std::u32string const& chosen() const { return chosen_; }
    /** The rows the list holds after the last listing or filter, so a caller
     *  can cue on what Volumes found. */
    int row_count() const;

    std::function<void(Action)> on_action;

    /** The list was re-filtered by the Pattern box: the wildcard it now holds
     *  and the row count it kept, so a caller can pace a read on it. */
    std::function<void(std::u32string const&, int)> on_filter;

    void show();
    void close();
    bool visible() const;

    Window& window() { return requester_.window(); }
    Application& application() { return app_; }

private:
    /** One listed entry: the name and what the volume's `list` answered. */
    struct Entry {
        std::u32string name;
        /* The second column while the list is the volumes: the literal word
         * "Assign" for a binding, empty for a volume (which shows its backing
         * device's name in `device` instead). */
        std::u32string detail;
        /* A volume's backing block device name, from the namespace Row. Empty
         * for a binding or a volume not on a block device. */
        std::u32string device;
        uint64_t size = 0;
        uint64_t kind = 0;
        uint64_t mtime = 0;
        /* A volume's capacity in bytes, from `space`; zero when unknown. */
        uint64_t total = 0;
        uint64_t free = 0;
        /* A binding row, not a volume: its second column is `detail`, and the
         * capacity columns are blank. */
        bool assign = false;
        bool directory() const { return kind == aegir::volume::kKindDir; }
    };

    void build_body();
    void list_drawer();
    void list_volumes();
    void apply_filter();
    /* Tell the scrollbar the list's rows and what it shows. The list's rectangle
     * is only known once it is laid out, so this also runs from the list's
     * on_visible_changed (specs/trinket/listview.md). */
    void sync_scrollbar();
    void set_drawer(std::u32string drawer);
    void choose(int row);
    Entry const* entry_named(std::u32string const& name) const;
    void progress(Action action);

    Application& app_;
    aegir::vfs::Namespace& vfs_;
    Requester requester_;
    ListView* list_ = nullptr;
    Scrollbar* scrollbar_ = nullptr;
    TextBox* pattern_box_ = nullptr;
    TextBox* drawer_box_ = nullptr;
    TextBox* file_box_ = nullptr;
    Button* drawer_toggle_ = nullptr;
    std::vector<Entry> entries_;
    std::u32string drawer_;
    std::u32string pattern_ = U"#?";
    std::u32string chosen_;
    bool showing_volumes_ = false;
    /* One slot, reused: a resolve mints the volume's caller half into it, and
     * the capability is deleted after the listing so the next resolve may mint
     * again (aegir-mem's allocator hands a slot out once). */
    seL4_CPtr resolve_slot_ = 0;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_FILE_REQUESTER_H
