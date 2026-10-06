/*
 * Host conformance for the process registry's table.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * scripts/check_process.py compiles this with the host compiler against
 * process_table.cc and runs it; it is not part of any target build. The table
 * is a value -- no seL4 -- so register, unregister, count/describe, the
 * attention flags and the authority check are asserted exactly
 * (specs/process.md).
 */

#include <aegir/ipc/badge.h>
#include <aegir/process.h>

#include "process_table.h"

#include <cstdio>
#include <cstring>

namespace {

using aegir::ipc::make_user_badge;
using aegir::process::kAttnAll;
using aegir::process::kAttnC;
using aegir::process::kAttnD;
using aegir::process::kBreakRefused;
using aegir::process::kBreakSet;
using aegir::process::kMethodBreak;
using aegir::process::kMethodCount;
using aegir::process::kMethodDescribe;
using aegir::process::kMethodRegister;
using aegir::process::kMethodUnregister;
using aegir::process::kProcessAdded;
using aegir::process::kProcessRefused;
using aegir::process::kRowWords;
using aegir::process::kStateBreakPending;
using aegir::process::kStateRunning;
using aegir::process::ProcessTable;
using aegir::process::Row;

int g_checks = 0;
int g_failures = 0;

void expect_u64(uint64_t got, uint64_t want, char const *what)
{
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr, "FAIL  %s: got %llu want %llu\n", what,
                     static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
    }
}

void expect_true(bool got, char const *what)
{
    expect_u64(got ? 1 : 0, 1, what);
}

/* A seed row the way the spawn kit would carry one. */
Row seed(uint64_t pid, uint64_t owner, char const *name, char const *path)
{
    Row row{};
    row.pid = pid;
    row.owner = owner;
    std::snprintf(row.name, sizeof(row.name), "%s", name);
    std::snprintf(row.path, sizeof(row.path), "%s", path);
    return row;
}

uint64_t const *words(Row const &row)
{
    return reinterpret_cast<uint64_t const *>(&row);
}

/* One `handle` call, answering the reply's first word (or a sentinel when the
 * envelope refused). */
uint64_t answer(ProcessTable &table, uint32_t method, uint64_t const *in, uint32_t count,
                uint64_t caller)
{
    uint64_t reply[kRowWords + 2] = {};
    uint32_t const written = table.handle(method, in, count, caller, reply, kRowWords + 2);
    if (method == kMethodDescribe) {
        return written == kRowWords ? reply[0] : ~0ULL;
    }
    return written >= 1 ? reply[0] : ~0ULL;
}

}  // namespace

int main()
{
    constexpr uint64_t kSystem = 512; /* a system badge: bit 62 clear */
    constexpr uint64_t kUser = make_user_badge(3, 1);
    constexpr uint64_t kOtherUser = make_user_badge(4, 1);

    Row storage[3];
    ProcessTable table(storage, 3);

    /* Empty. */
    expect_u64(answer(table, kMethodCount, nullptr, 0, kUser), 0, "an empty table counts zero");

    /* Register two: one user's, one the system's. */
    Row a = seed(7001, kUser, "editor", "Sys:C/edit");
    Row b = seed(7002, kSystem, "net", "Sys:C/net");
    expect_u64(answer(table, kMethodRegister, words(a), kRowWords, kUser), kProcessAdded,
               "a user's process registers");
    expect_u64(answer(table, kMethodRegister, words(b), kRowWords, kSystem), kProcessAdded,
               "a system-started process registers");
    expect_u64(table.count(), 2, "the table holds two");

    /* A duplicate pid is refused; the first row is kept. */
    expect_u64(answer(table, kMethodRegister, words(a), kRowWords, kUser), kProcessRefused,
               "a duplicate pid is refused");

    /* Describe walks the set; an index past the count is the empty reply. */
    Row const *first = table.at(0);
    expect_true(first != nullptr && first->pid == 7001, "describe names the first pid");
    expect_u64(answer(table, kMethodCount, nullptr, 0, kUser), 2, "count answers two");
    uint64_t past = 9;
    uint64_t scratch[kRowWords + 2] = {};
    expect_u64(table.handle(kMethodDescribe, &past, 1, kUser, scratch, kRowWords + 2), 0,
               "describe past the count is the empty reply");

    /* A user breaks its own class: C sets the flag and leaves it pending. */
    uint64_t brk[2] = {7001, kAttnC};
    expect_u64(answer(table, kMethodBreak, brk, 2, kUser), kBreakSet, "a user breaks its own");
    expect_true(table.find(7001) != nullptr && table.find(7001)->flags == kAttnC,
                "the C flag is set");
    expect_true(table.find(7001)->state == kStateBreakPending, "C leaves it break-pending");

    /* A user cannot break another user's process. */
    uint64_t brk_other[2] = {7001, kAttnD};
    expect_u64(answer(table, kMethodBreak, brk_other, 2, kOtherUser), kBreakRefused,
               "a user cannot break another user's process");
    expect_true(table.find(7001)->flags == kAttnC, "the refused flag did not land");

    /* A user cannot break a system-started process; the system class can. */
    uint64_t brk_sys[2] = {7002, kAttnC};
    expect_u64(answer(table, kMethodBreak, brk_sys, 2, kUser), kBreakRefused,
               "a user cannot break a system-started process");
    expect_u64(answer(table, kMethodBreak, brk_sys, 2, kSystem), kBreakSet,
               "the system class breaks a system process");

    /* An unknown pid is refused. */
    uint64_t brk_none[2] = {9999, kAttnC};
    expect_u64(answer(table, kMethodBreak, brk_none, 2, kUser), kBreakRefused,
               "an unknown pid is refused");

    /* Only the known flags are kept. */
    uint64_t brk_all[2] = {7001, ~0ULL};
    expect_u64(answer(table, kMethodBreak, brk_all, 2, kUser), kBreakSet, "all flags set");
    expect_true(table.find(7001)->flags == kAttnAll, "only the known flags are kept");

    /* Unregister removes it; the hole is filled and find misses it. */
    uint64_t gone = 7001;
    expect_u64(answer(table, kMethodUnregister, &gone, 1, kUser), kProcessAdded,
               "unregister removes a pid");
    expect_u64(table.count(), 1, "one is left");
    expect_true(table.find(7001) == nullptr, "the removed pid is gone");

    /* A full table refuses rather than growing. */
    Row c = seed(7003, kUser, "shell", "Sys:C/shell");
    Row d = seed(7004, kUser, "make", "Sys:C/make");
    Row e = seed(7005, kUser, "more", "Sys:C/more");
    expect_u64(answer(table, kMethodRegister, words(c), kRowWords, kUser), kProcessAdded,
               "a third registers");
    expect_u64(answer(table, kMethodRegister, words(d), kRowWords, kUser), kProcessAdded,
               "a fourth fills it");
    expect_u64(answer(table, kMethodRegister, words(e), kRowWords, kUser), kProcessRefused,
               "a full table refuses");

    /* A name or path past its field is cut and NUL-terminated, not read past. */
    Row long_name{};
    long_name.pid = 8001;
    long_name.owner = kUser;
    std::memset(long_name.name, 'x', sizeof(long_name.name));
    std::memset(long_name.path, 'y', sizeof(long_name.path));
    Row small[1];
    ProcessTable one(small, 1);
    expect_u64(answer(one, kMethodRegister, words(long_name), kRowWords, kUser), kProcessAdded,
               "the long one registers");
    expect_u64(small[0].name[sizeof(small[0].name) - 1], 0, "the long name is terminated");
    expect_u64(small[0].path[sizeof(small[0].path) - 1], 0, "the long path is terminated");

    /* An unknown method is answered by saying nothing. */
    expect_u64(answer(table, 99, nullptr, 0, kUser), ~0ULL, "an unknown method says nothing");

    /* The state a fresh row carries. */
    expect_u64(small[0].state, kStateRunning, "a new row runs");
    expect_u64(small[0].flags, 0, "a new row has no flags");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
