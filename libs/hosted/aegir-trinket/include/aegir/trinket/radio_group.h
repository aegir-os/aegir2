/*
 * Trinket RadioGroup - a group of radio buttons with one selection.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * MUI's Radio is a framed group whose children are mutually exclusive
 * (specs/trinket/radio_group.md): choosing one clears the others. The group owns
 * the selection; a member only knows it was clicked, and a member added alone
 * behaves like a checkbox.
 */

#ifndef AEGIR_TRINKET_RADIO_GROUP_H
#define AEGIR_TRINKET_RADIO_GROUP_H

#include <aegir/trinket/button.h>
#include <aegir/trinket/group.h>
#include <functional>
#include <string_view>
#include <vector>

namespace aegir::trinket {

class RadioGroup : public Group {
public:
    explicit RadioGroup(Orientation orientation = Orientation::VERTICAL,
                        int spacing = 0);
    ~RadioGroup() override;

    /* Adds a radio button and joins it to the group. The first one added is the
     * active one, so a group is never left with nothing selected; the returned
     * button is the group's, to name or disable. */
    Button* add(std::u32string_view text);
    Button* add(std::string_view text);

    /* Which member is checked, or -1 for none. Setting it clears the others and
     * reports `on_changed`. */
    void set_active(int index);
    int active() const { return active_; }

    int count() const { return static_cast<int>(members_.size()); }

    /* The active member changed: by click, by arrow key, or by `set_active`. */
    std::function<void(int)> on_changed;

    /* The group takes the focus so the arrow keys move the selection without a
     * member having to be focused first (specs/trinket/radio_group.md). */
    bool focusable() const override { return true; }

protected:
    void on_key_down(const KeyEvent& event) override;

private:
    void select(Button* button);

    /* The members, in order. Non-owning: `children_` owns them, and a group's
     * members are not removed while it lives. */
    std::vector<Button*> members_;
    int active_ = -1;
};

}  // namespace aegir::trinket

#endif  // AEGIR_TRINKET_RADIO_GROUP_H
