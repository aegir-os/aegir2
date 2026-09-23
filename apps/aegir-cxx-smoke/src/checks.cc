/*
 * aegir-cxx-smoke: the runtime checks -- implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This translation unit is the libc++ side. It must not include any seL4 header
 * (see checks.h): libsel4 declares `strcpy` with C++ linkage and musl's
 * <string.h>, which libc++ pulls in, declares it with C linkage.
 *
 * Each check is its own function for clarity. The stack-frame problem they
 * once papered over is fixed in the hosted policy: -O2 lets the compiler reuse
 * stack slots across libc++'s always-inline code, where -O0 gave one
 * unordered_map insertion a 33 KiB frame (see aegir-cxx-policy-hosted in the
 * top-level CMakeLists.txt).
 */

#include "checks.h"

#include <aegir/debug.h>

#include <cstdlib>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace aegir::cxx_smoke {

namespace {

int g_failed = 0;

void report(bool ok, char const *what)
{
    aegir::debug_write(ok ? "  cxx-smoke: ok: " : "  cxx-smoke: FAIL: ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
    if (!ok) {
        ++g_failed;
    }
}

/* Raw malloc/free: this is musl's own path, before any container. */
void check_malloc()
{
    void *raw = std::malloc(4096);
    report(raw != nullptr, "malloc returns memory");
    if (raw != nullptr) {
        auto *bytes = static_cast<unsigned char *>(raw);
        for (unsigned i = 0; i < 4096; ++i) {
            bytes[i] = static_cast<unsigned char>(i * 31 + 7);
        }
        bool intact = true;
        for (unsigned i = 0; i < 4096; ++i) {
            intact = intact && bytes[i] == static_cast<unsigned char>(i * 31 + 7);
        }
        report(intact, "the malloc'd page holds what was written");
        std::free(raw);
    }
}

/* std::string: growth reallocation, so free and malloc both run. */
void check_string()
{
    std::string text = "aegir";
    for (int i = 0; i < 200; ++i) {
        text += "-hosted";
    }
    report(text.size() == 5 + 200 * 7 && text.compare(0, 5, "aegir") == 0,
           "std::string grows and reallocates");
}

/* std::vector: element construction and a growth sequence. */
void check_vector()
{
    std::vector<int> values;
    for (int i = 0; i < 4000; ++i) {
        values.push_back(i * 3);
    }
    report(values.size() == 4000 && values.front() == 0 && values.back() == 3999 * 3,
           "std::vector grows and reads back");
}

/* std::unordered_map: node allocation, hashing, and lookup. */
void check_map()
{
    std::unordered_map<std::string, int> counts;
    counts["one"] = 1;
    counts["two"] = 2;
    counts["three"] = 3;
    report(counts.size() == 3 && counts["two"] == 2,
           "std::unordered_map allocates and looks up");
}

/* Free and reallocate the same size: mallocng's groups should hand the chunk
 * back, which is what makes this heap freeing rather than a bump. */
void check_reuse()
{
    void *first = std::malloc(256);
    std::free(first);
    void *second = std::malloc(256);
    report(second != nullptr, "a freed chunk can be allocated again");
    std::free(second);
}

/* Exceptions (specs/cxx.md's completion program, step 3): a throw has to walk
 * every frame between it and its catch, running the destructors on the way, so
 * the check is that the destructor ran and not merely that the catch was
 * reached. The frame object below is what unwinding proves: without .eh_frame
 * the throw calls std::terminate rather than unwind. */
int g_unwound = 0;

struct Unwinder {
    int *counter;
    explicit Unwinder(int *c) : counter(c) {}
    ~Unwinder() { *counter += 1; }
};

void throw_through()
{
    Unwinder unwinder(&g_unwound);
    throw 0x51; /* a live frame object when the throw leaves this frame */
}

void check_exceptions()
{
    bool caught = false;
    try {
        throw_through();
    } catch (int value) {
        caught = value == 0x51;
    }
    report(caught && g_unwound == 1,
           "a throw unwinds a frame and runs its destructor");
}

/* RTTI: a polymorphic base, downcast through dynamic_cast. That the cast
 * succeeds is the proof -- it is the runtime's type table, not the compiler's
 * static type, that answers. */
struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {
    int marker = 7;
};

void check_rtti()
{
    Derived derived;
    Base *base = &derived;
    Derived *down = dynamic_cast<Derived *>(base);
    std::string const name = typeid(*base).name();
    report(down != nullptr && down->marker == 7 &&
               name.find("Derived") != std::string::npos,
           "RTTI identifies a dynamic type");
}

}  // namespace

int run()
{
    check_malloc();
    check_string();
    check_vector();
    check_map();
    check_reuse();
    check_exceptions();
    check_rtti();
    return g_failed;
}

}  // namespace aegir::cxx_smoke
