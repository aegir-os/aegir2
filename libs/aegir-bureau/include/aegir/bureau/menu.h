/*
 * The bureau.menu port: an app registering the menus the screen bar shows.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * The screen bar shows the **active window's** menus (specs/workbench.md).
 * The console sends a focus event only to a window's owner, so the client --
 * which owns its window -- is the one that knows when its menus should stand,
 * and the one that must tell the bureau. Three calls make the whole of it:
 * a client registers its tree once, reports its focus as it changes, and
 * fetches an action when the bureau rings the doorbell it was given.
 *
 * The doorbell is the notification the client already listens on -- the
 * console's, from `console::listen` -- and it rides to the bureau as the
 * capability `register` transfers. The bureau keeps it, and when an item is
 * clicked it stores the action and signals that same notification: the client
 * is already woken by it, drains the console's ring, and calls `take_action`
 * to fetch what was stored. The bureau never calls into the client, because a
 * single-threaded client cannot be mid-call and in its event loop at once
 * (specs/workbench.md).
 *
 * The model is the toolkit's `MenuBar::Menu`, so the bureau draws what it
 * already draws. Strings travel as `std::u32string` code points, two to a
 * word (low half first); a menu is a count and its title, an item its action
 * id, flags and label. The tree rides in one call, so the kernel's message
 * registers are the ceiling (`ipc::kMaxWords`, the envelope's own); a tree
 * past them is refused, and a shared-window form is the escape hatch when a
 * real one needs it.
 */

#ifndef AEGIR_BUREAU_MENU_H
#define AEGIR_BUREAU_MENU_H

#include <aegir/ipc/port.h>
#include <aegir/trinket/menubar.h>
#include <sel4/sel4.h>
#include <stdint.h>
#include <vector>

namespace aegir::bureau::menu {

using aegir::trinket::MenuBar;

/** The port's name, in the class.instance shape every port is named in. */
constexpr char const kPortName[] = "bureau.menu";
constexpr uint32_t kPortNameLength = sizeof(kPortName) - 1;

/** Register: in: the menu tree, encoded below, and -- by capability transfer
 *  -- a signal-capable copy of the client's doorbell. Answer: one word, 1
 *  when the tree was taken and 0 when refused (it did not decode, or it did
 *  not fit). The tree replaces whatever that client last registered. */
constexpr uint32_t kMethodRegister = 1;

/** Set active: in: one word, 1 while the caller's window has the focus and 0
 *  when it loses it. The bureau shows that client's menus in place of its own
 *  while it is active, and its own when none is. Answer: empty. */
constexpr uint32_t kMethodSetActive = 2;

/** Take action: in: nothing. Answer: one word -- the action id of an item
 *  clicked since the last fetch, or 0 when there is none. Action ids are the
 *  client's own, and 0 is reserved for "none" (the model's separators carry
 *  0 and are never actionable, so nothing is lost). */
constexpr uint32_t kMethodTakeAction = 3;

/** How many words a string of `code_points` code points takes: two to a
 *  word, the odd one filling the low half. */
constexpr uint32_t string_words(uint32_t code_points) noexcept
{
    return (code_points + 1) / 2;
}

/** Encode a menu tree into `out` (at most `capacity` words). False when it
 *  does not fit, in which case nothing was promised -- `out` may hold a
 *  partial tree and `count` is not written. */
inline bool encode(std::vector<MenuBar::Menu> const &menus, uint64_t *out,
                   uint32_t capacity, uint32_t *count) noexcept
{
    uint32_t at = 0;
    auto put = [&](uint64_t word) {
        if (at < capacity) {
            out[at] = word;
        }
        ++at;
    };
    put(menus.size());
    for (MenuBar::Menu const &menu : menus) {
        put(menu.items.size());
        put(menu.title.size());
        for (uint32_t i = 0; i < menu.title.size(); i += 2) {
            uint64_t word = menu.title[i];
            if (i + 1 < menu.title.size()) {
                word |= static_cast<uint64_t>(menu.title[i + 1]) << 32;
            }
            put(word);
        }
        for (MenuBar::MenuItem const &item : menu.items) {
            put(item.action_id);
            put(item.flags);
            put(item.label.size());
            for (uint32_t i = 0; i < item.label.size(); i += 2) {
                uint64_t word = item.label[i];
                if (i + 1 < item.label.size()) {
                    word |= static_cast<uint64_t>(item.label[i + 1]) << 32;
                }
                put(word);
            }
        }
    }
    if (at > capacity) {
        return false;
    }
    *count = at;
    return true;
}

/** Decode a tree `encode` wrote. False when the words do not read as one --
 *  a count past the words that follow it, or a string past them. The counts
 *  are checked against the words that remain before anything is allocated, so
 *  a malformed tree cannot ask for memory it never brought. */
inline bool decode(uint64_t const *in, uint32_t count,
                   std::vector<MenuBar::Menu> *out) noexcept
{
    uint32_t at = 0;
    auto get = [&](uint64_t *word) {
        if (at >= count) {
            return false;
        }
        *word = in[at++];
        return true;
    };
    uint64_t menu_count = 0;
    if (!get(&menu_count) || menu_count > count) {
        return false;
    }
    out->clear();
    out->reserve(menu_count);
    for (uint64_t mi = 0; mi < menu_count; ++mi) {
        uint64_t item_count = 0;
        uint64_t title_length = 0;
        if (!get(&item_count) || !get(&title_length)) {
            return false;
        }
        if (item_count > count || title_length > 2ull * count) {
            return false;
        }
        MenuBar::Menu menu;
        menu.title.resize(title_length);
        for (uint32_t i = 0; i < title_length; i += 2) {
            uint64_t word = 0;
            if (!get(&word)) {
                return false;
            }
            menu.title[i] = static_cast<char32_t>(word & 0xffffffffu);
            if (i + 1 < title_length) {
                menu.title[i + 1] = static_cast<char32_t>(word >> 32);
            }
        }
        menu.items.resize(item_count);
        for (uint64_t ii = 0; ii < item_count; ++ii) {
            uint64_t action = 0;
            uint64_t flags = 0;
            uint64_t label_length = 0;
            if (!get(&action) || !get(&flags) || !get(&label_length)) {
                return false;
            }
            if (label_length > 2ull * count) {
                return false;
            }
            MenuBar::MenuItem item;
            item.action_id = static_cast<uint32_t>(action);
            item.flags = static_cast<uint8_t>(flags);
            item.label.resize(label_length);
            for (uint32_t i = 0; i < label_length; i += 2) {
                uint64_t word = 0;
                if (!get(&word)) {
                    return false;
                }
                item.label[i] = static_cast<char32_t>(word & 0xffffffffu);
                if (i + 1 < label_length) {
                    item.label[i + 1] = static_cast<char32_t>(word >> 32);
                }
            }
            menu.items[ii] = std::move(item);
        }
        out->push_back(std::move(menu));
    }
    return true;
}

/* The client walk, the console.h shape: small inline callers over the port's
 * Consumer. */

/** Register `menus` and hand the bureau `doorbell`. False when the call was
 *  refused or the tree did not fit; `cap_received` says whether the bureau
 *  took the capability (a call that expected one and got none is a missing
 *  Grant, and the answer says so). */
inline bool register_menus(aegir::ipc::Consumer const &bureau,
                           std::vector<MenuBar::Menu> const &menus,
                           seL4_CPtr doorbell, bool *cap_received = nullptr) noexcept
{
    uint64_t words[aegir::ipc::kMaxWords];
    uint32_t count = 0;
    if (!encode(menus, words, aegir::ipc::kMaxWords, &count)) {
        return false;
    }
    uint64_t in[1];
    aegir::ipc::WordsReply const answer = bureau.call_transfer(
        kMethodRegister, words, count, doorbell, in, 1, cap_received);
    return answer.error == 0 && answer.count == 1 && in[0] != 0;
}

/** Report the caller's window focus. False when the call was refused. */
inline bool set_active(aegir::ipc::Consumer const &bureau, bool active) noexcept
{
    aegir::ipc::Reply const answer = bureau.call(kMethodSetActive, active ? 1 : 0);
    return answer.error == 0;
}

/** Fetch the action clicked since the last fetch, or 0 when none. */
inline uint32_t take_action(aegir::ipc::Consumer const &bureau) noexcept
{
    aegir::ipc::Reply const answer = bureau.call(kMethodTakeAction, 0);
    if (answer.error != 0) {
        return 0;
    }
    return static_cast<uint32_t>(answer.word);
}

}  // namespace aegir::bureau::menu

#endif  // AEGIR_BUREAU_MENU_H
