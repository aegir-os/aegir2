/*
 * Trinket FileRequester implementation (see the header).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 */

#include <aegir/trinket/file_requester.h>

#include <aegir/trinket/button.h>
#include <aegir/trinket/file_path.h>
#include <aegir/trinket/group.h>
#include <aegir/trinket/icon.h>
#include <aegir/trinket/label.h>
#include <aegir/trinket/locale.h>
#include <aegir/trinket/listview.h>
#include <aegir/trinket/scrollbar.h>
#include <aegir/trinket/textbox.h>
#include <aegir/trinket/theme.h>
#include <aegir/trinket/unicode.h>

#include <aegir/bootstrap.h>
#include <aegir/pattern.h>
#include <sel4/sel4.h>

#include <utility>

namespace aegir::trinket {

namespace {
/* The body's own shape: a wide list over the three control rows. */
constexpr int kBodyWidth = 440;
constexpr int kListRows = 10;
}  // namespace

FileRequester::FileRequester(Application& app, aegir::vfs::Namespace& vfs,
                             std::u32string_view title)
    : app_(app), vfs_(vfs),
      requester_(app, title,
                 std::vector<Requester::Action>{
                     {OK, U"OK", true, false},
                     {VOLUMES, U"Volumes", false, false},
                     {PARENT, U"Parent", false, false},
                     {CANCEL, U"Cancel", false, true},
                 })
{
    /* Volumes and Parent act on the open dialog; OK and Cancel close it. */
    requester_.set_stays_open(VOLUMES);
    requester_.set_stays_open(PARENT);
    requester_.on_action = [this](uint32_t id) { progress(static_cast<Action>(id)); };
    resolve_slot_ = app_.alloc_slot();
    build_body();
}

FileRequester::FileRequester(Application& app, aegir::vfs::Namespace& vfs,
                             std::string_view title)
    : FileRequester(app, vfs, utf8_to_utf32(title))
{
}

FileRequester::~FileRequester() = default;

void FileRequester::build_body()
{
    Theme& theme = app_.theme();
    int const gap = theme.metric(MetricRole::SPACING_SMALL);

    /* The list: a titles row and the entries, name/size/date/time, with the
     * drawer icons. The name column is the free one (width 0). */
    auto list = std::make_unique<ListView>();
    list_ = list.get();
    list_->set_columns({
        {U"Name", 0, ListView::Alignment::LEFT},
        {U"Size", 72, ListView::Alignment::RIGHT},
        {U"Date", 100, ListView::Alignment::LEFT},
        {U"Time", 72, ListView::Alignment::LEFT},
    });
    list_->on_select = [this](int row) { choose(row); };

    auto scrollbar = std::make_unique<Scrollbar>(Scrollbar::Orientation::VERTICAL);
    scrollbar_ = scrollbar.get();
    scrollbar_->on_scroll = [this](int first) {
        if (list_ != nullptr) {
            list_->set_first(first);
        }
    };
    auto list_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, 0);
    Group* const list_row_ptr = list_row.get();
    list_row->add_child(std::move(list));
    list_row->add_child(std::move(scrollbar));
    list_row->set_weight(scrollbar_, 0);

    /* The Pattern box: a keystroke filters the rows already listed. */
    auto pattern = std::make_unique<TextBox>();
    pattern_box_ = pattern.get();
    pattern_box_->set_text(pattern_);
    pattern_box_->on_text_changed = [this](std::u32string_view text) {
        pattern_ = std::u32string(text);
        apply_filter();
    };
    auto pattern_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, gap);
    Group* const pattern_row_ptr = pattern_row.get();
    pattern_row->add_child(std::make_unique<Label>(U"Pattern"));
    pattern_row->add_child(std::move(pattern));
    pattern_row->set_weight(pattern_box_, 100);

    /* The Drawer toggle and the path box: checked keeps the drawers listed,
     * unchecked hides them, and Enter in the box opens the path. */
    auto toggle = std::make_unique<Button>(U"Drawer", Button::Type::CHECK);
    drawer_toggle_ = toggle.get();
    drawer_toggle_->set_checked(true);
    drawer_toggle_->on_click = [this](bool) { apply_filter(); };
    auto drawer = std::make_unique<TextBox>();
    drawer_box_ = drawer.get();
    drawer_box_->set_text(drawer_);
    drawer_box_->on_submit = [this]() {
        if (drawer_box_ != nullptr) {
            set_drawer(drawer_box_->text());
        }
    };
    auto drawer_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, gap);
    Group* const drawer_row_ptr = drawer_row.get();
    drawer_row->add_child(std::move(toggle));
    drawer_row->add_child(std::move(drawer));
    drawer_row->set_weight(drawer_box_, 100);

    /* The File box: the chosen name, typed or set by a row's click. */
    auto file = std::make_unique<TextBox>();
    file_box_ = file.get();
    file_box_->on_submit = [this]() {
        chosen_ = file_box_ != nullptr ? file_box_->text() : std::u32string();
        progress(OK);
    };
    auto file_row = std::make_unique<Group>(Group::Orientation::HORIZONTAL, gap);
    Group* const file_row_ptr = file_row.get();
    file_row->add_child(std::make_unique<Label>(U"File"));
    file_row->add_child(std::move(file));
    file_row->set_weight(file_box_, 100);

    auto body = std::make_unique<Group>(Group::Orientation::VERTICAL, gap);
    body->add_child(std::move(list_row));
    body->add_child(std::move(pattern_row));
    body->add_child(std::move(drawer_row));
    body->add_child(std::move(file_row));
    /* The list is free and takes the height; the control rows keep their bands. */
    body->set_weight(list_row_ptr, 100);
    body->set_weight(pattern_row_ptr, 0);
    body->set_weight(drawer_row_ptr, 0);
    body->set_weight(file_row_ptr, 0);

    int const list_height = list_->height_for_rows(kListRows);
    int const control = pattern_row_ptr->preferred_size().height;
    Size const size{kBodyWidth, list_height + 3 * (control + gap)};
    requester_.set_content(std::move(body), size);
}

void FileRequester::open_at(std::u32string_view drawer)
{
    set_drawer(std::u32string(drawer));
}

void FileRequester::set_pattern(std::u32string_view pattern)
{
    pattern_ = std::u32string(pattern);
    if (pattern_box_ != nullptr) {
        pattern_box_->set_text(pattern_);
    }
    apply_filter();
}

void FileRequester::set_drawer(std::u32string drawer)
{
    drawer_ = std::move(drawer);
    if (drawer_box_ != nullptr) {
        drawer_box_->set_text(drawer_);
    }
    list_drawer();
}

void FileRequester::list_drawer()
{
    entries_.clear();
    showing_volumes_ = false;
    if (vfs_.valid() && resolve_slot_ != 0 && !drawer_.empty()) {
        std::string const path = utf32_to_utf8(drawer_);
        aegir::vfs::Namespace::Resolved resolved{};
        if (vfs_.resolve(path.c_str(), static_cast<uint32_t>(path.size()), resolve_slot_,
                         resolved)) {
            aegir::vfs::Volume volume(resolved.volume);
            for (uint64_t index = 0;; ++index) {
                aegir::vfs::Volume::Entry entry{};
                if (!volume.list(resolved.rest, resolved.rest_length, index, entry)) {
                    break;
                }
                Entry e;
                e.name = utf8_to_utf32(std::string_view(entry.name, entry.name_length));
                e.size = entry.size;
                e.kind = entry.kind;
                e.mtime = entry.mtime;
                entries_.push_back(std::move(e));
            }
        }
        /* The minted capability is ours to drop: a slot is handed out once, and
         * emptying it lets the next resolve mint into it again. */
        seL4_CNode_Delete(aegir::bootstrap::kSlotOwnCNode, resolve_slot_,
                          aegir::bootstrap::cnode_bits());
    }
    apply_filter();
}

void FileRequester::list_volumes()
{
    entries_.clear();
    showing_volumes_ = true;
    uint64_t count = 0;
    if (vfs_.valid() && vfs_.volume_count(count)) {
        for (uint64_t index = 0; index < count; ++index) {
            aegir::nmspace::Row row{};
            if (!vfs_.describe(index, row)) {
                continue;
            }
            /* A volume with no directory to browse -- NIL:, PIPE: -- is not a
             * place the requester can go (specs/vfs.md). */
            if ((row.flags & aegir::nmspace::kFlagNoDir) != 0) {
                continue;
            }
            Entry e;
            e.name = utf8_to_utf32(std::string_view(row.name));
            e.kind = aegir::volume::kKindDir;
            entries_.push_back(std::move(e));
        }
    }
    apply_filter();
}

void FileRequester::apply_filter()
{
    if (list_ == nullptr) {
        return;
    }
    bool const show_drawers =
        showing_volumes_ || drawer_toggle_ == nullptr || drawer_toggle_->checked();
    std::string const pattern = utf32_to_utf8(pattern_);
    list_->clear();
    int row = 0;
    for (Entry const& e : entries_) {
        if (e.directory() && !show_drawers) {
            continue;
        }
        std::string const name = utf32_to_utf8(e.name);
        if (!aegir::pattern::match(pattern.c_str(), static_cast<uint32_t>(pattern.size()),
                                   name.c_str(), static_cast<uint32_t>(name.size()))) {
            continue;
        }
        std::vector<std::u32string> cells;
        cells.push_back(e.name);
        cells.push_back(e.directory() ? std::u32string()
                                      : utf8_to_utf32(file_path::format_size(e.size)));
        cells.push_back(e.mtime != 0
                            ? utf8_to_utf32(app_.locale().format_date(
                                  static_cast<int64_t>(e.mtime)))
                            : std::u32string());
        cells.push_back(e.mtime != 0
                            ? utf8_to_utf32(app_.locale().format_time(
                                  static_cast<int64_t>(e.mtime)))
                            : std::u32string());
        list_->set_row(row, std::move(cells), e.directory() ? Icon::DRAWER : Icon::NONE);
        ++row;
    }
    list_->set_first(0);
    if (scrollbar_ != nullptr) {
        scrollbar_->set_range(list_->count(), list_->visible_rows());
        scrollbar_->set_value(list_->first());
    }
}

FileRequester::Entry const* FileRequester::entry_named(std::u32string const& name) const
{
    for (Entry const& e : entries_) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

void FileRequester::choose(int row)
{
    if (list_ == nullptr || row < 0 || row >= list_->count()) {
        return;
    }
    std::u32string const name = list_->cell(row, 0);
    if (name.empty()) {
        return;
    }
    /* A volume row (or a drawer row) opens the drawer; a file fills the File
     * box, which OK then reports. */
    if (showing_volumes_) {
        set_drawer(name + U":");
        return;
    }
    Entry const* const entry = entry_named(name);
    if (entry != nullptr && entry->directory()) {
        set_drawer(file_path::join(drawer_, name));
        return;
    }
    chosen_ = name;
    if (file_box_ != nullptr) {
        file_box_->set_text(name);
    }
}

void FileRequester::progress(Action action)
{
    switch (action) {
    case OK:
        chosen_ = file_box_ != nullptr ? file_box_->text() : chosen_;
        break;
    case VOLUMES:
        list_volumes();
        break;
    case PARENT:
        set_drawer(file_path::parent_of(drawer_));
        break;
    case CANCEL:
        chosen_.clear();
        break;
    }
    if (on_action) {
        on_action(action);
    }
}

void FileRequester::show()
{
    /* List again on every show, so a second open is fresh. */
    if (drawer_.empty()) {
        list_volumes();
    } else {
        list_drawer();
    }
    requester_.show();
}

void FileRequester::close()
{
    requester_.close();
}

bool FileRequester::visible() const
{
    return requester_.visible();
}

}  // namespace aegir::trinket
