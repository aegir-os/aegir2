/*
 * Host conformance for the file requester's path and size helpers
 * (specs/trinket/file_requester.md).
 *
 * The drawer above a path, the join of a drawer and a name, and a size as the
 * AmigaDOS `List` writes it: pure strings, so synthetic ones are enough, and a
 * wrong parent or a missing separator is exactly what a screen shows only by
 * luck.
 *
 * Host tools only (python3 and a C++ compiler); not part of any target build.
 * Run through scripts/check_file_path.py.
 */

#include <aegir/trinket/file_path.h>

#include <cstdio>
#include <string>

namespace {

using aegir::trinket::file_path::format_size;
using aegir::trinket::file_path::join;
using aegir::trinket::file_path::parent_of;

unsigned g_checks = 0;
unsigned g_failures = 0;

/* The paths are ASCII, so a narrow view is enough for a failure message. */
std::string narrow(std::u32string_view text) {
    std::string out;
    for (char32_t const c : text) {
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}

void check(bool ok, std::string const &what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

void expect_parent(std::u32string const &path, std::u32string const &want,
                   std::string const &what) {
    std::u32string const got = parent_of(path);
    check(got == want, what + ": got " + narrow(got) + ", want " + narrow(want));
}

void expect_join(std::u32string const &drawer, std::u32string const &name,
                 std::u32string const &want, std::string const &what) {
    std::u32string const got = join(drawer, name);
    check(got == want, what + ": got " + narrow(got) + ", want " + narrow(want));
}

void expect_size(uint64_t bytes, std::string const &want, std::string const &what) {
    std::string const got = format_size(bytes);
    check(got == want, what + ": got " + got + ", want " + want);
}

}  // namespace

int main() {
    /* The parent of a path drops its last component. */
    expect_parent(U"", U"", "an empty path has no parent");
    expect_parent(U"AEGIR:", U"AEGIR:", "a volume root is its own parent");
    expect_parent(U"AEGIR:Docs", U"AEGIR:", "a volume's first component's parent is the root");
    expect_parent(U"AEGIR:Docs/Sub", U"AEGIR:Docs", "a nested component's parent is the one above");
    expect_parent(U"AEGIR:Docs/", U"AEGIR:", "a trailing separator is not a component");
    expect_parent(U"AEGIR:Docs/Sub/", U"AEGIR:Docs", "and it is dropped before the split");

    /* The join takes the separator the drawer needs. */
    expect_join(U"", U"X", U"X", "an empty drawer is the name alone");
    expect_join(U"AEGIR:", U"X", U"AEGIR:X", "a volume root takes no separator");
    expect_join(U"AEGIR:Docs", U"X", U"AEGIR:Docs/X", "a drawer takes a slash");
    expect_join(U"AEGIR:Docs/", U"X", U"AEGIR:Docs/X", "a trailing slash is not doubled");
    expect_join(U"AEGIR:", U"", U"AEGIR:", "an empty name is the drawer alone");

    /* The size is a comma every three digits. */
    expect_size(0, "0", "zero has no comma");
    expect_size(999, "999", "three digits have none");
    expect_size(1000, "1,000", "four digits have one");
    expect_size(2136, "2,136", "the sample the MUI screenshot shows");
    expect_size(1000000, "1,000,000", "seven digits have two");

    std::printf("file-path: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
