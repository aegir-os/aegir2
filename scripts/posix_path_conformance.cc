/*
 * Host conformance for the POSIX path view (specs/posix.md).
 *
 * The translation is a pure function of a path and the current directory: the
 * spec's table (`/Name/rest` is `Name:rest`, `/` is the synthetic root), the
 * `.` and `..` normalization the Amiga's directories force, the native path
 * that passes through untouched, and the reverse that `getcwd` and a listing
 * present. A boot proves the view end to end; this proves the rules, where a
 * mistake would land as a path that resolves to the wrong volume rather than
 * as a line.
 */

#include <aegir/posix/path.h>

#include <cstdint>
#include <cstdio>

namespace {

using aegir::posix::path::Target;
using aegir::posix::path::Translation;

/* The namespace's own bound (aegir/nmspace.h's kPathMax), which is what the
 * layer's buffers are: the view names no ceiling of its own. */
constexpr uint32_t kRoom = 944;

unsigned g_checks = 0;
unsigned g_failures = 0;

void report(bool ok, char const *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

uint32_t text_length(char const *text)
{
    uint32_t length = 0;
    while (text != nullptr && text[length] != '\0') {
        ++length;
    }
    return length;
}

bool same(char const *first, uint32_t first_length, char const *second,
          uint32_t second_length)
{
    if (first_length != second_length) {
        return false;
    }
    for (uint32_t i = 0; i < first_length; ++i) {
        if (first[i] != second[i]) {
            return false;
        }
    }
    return true;
}

Translation translate(char const *path, char const *cwd, char *got, uint32_t capacity)
{
    Translation out{Target::Root, 0};
    if (!aegir::posix::path::translate(path, text_length(path), cwd, text_length(cwd), got,
                                       capacity, out)) {
        out = Translation{Target::Root, 0};
    }
    return out;
}

/* One path the view should answer with a VFS path. */
void expect_vfs(char const *path, char const *cwd, char const *want, char const *what)
{
    char got[kRoom];
    Translation const out = translate(path, cwd, got, kRoom);
    report(out.target == Target::Vfs && same(got, out.length, want, text_length(want)),
           what);
}

/* One path the view should answer with the synthetic root. */
void expect_root(char const *path, char const *cwd, char const *what)
{
    char got[kRoom];
    Translation const out = translate(path, cwd, got, kRoom);
    report(out.target == Target::Root && out.length == 0, what);
}

/* One path the view should refuse, with the room it was given. */
void expect_refused(char const *path, char const *cwd, uint32_t capacity,
                    char const *what)
{
    char got[kRoom];
    Translation out{Target::Root, 0};
    report(!aegir::posix::path::translate(path, text_length(path), cwd, text_length(cwd),
                                          got, capacity, out),
           what);
}

/* The reverse: a VFS path as the POSIX view presents it. */
void expect_present(char const *vfs, char const *want, char const *what)
{
    char got[kRoom];
    uint32_t length = 0;
    bool const ok = aegir::posix::path::present(vfs, text_length(vfs), got, kRoom, length);
    report(ok && same(got, length, want, text_length(want)), what);
}

void expect_present_refused(char const *vfs, uint32_t capacity, char const *what)
{
    char got[kRoom];
    uint32_t length = 0;
    report(!aegir::posix::path::present(vfs, text_length(vfs), got, capacity, length), what);
}

}  // namespace

int main()
{
    char const *const cwd = "/Sys/Tests";

    /* The root, and the `..` that reaches it. */
    {
        expect_root("/", cwd, "`/` is the synthetic root");
        expect_root("/.", cwd, "`/.` is the root");
        expect_root("/..", cwd, "`..` at the root is the root");
        expect_root("/Sys/..", cwd, "`..` above a volume's root is the root");
        expect_root("/Sys/DOCS/../..", cwd, "two `..` out of a volume is the root");
        expect_root(".", "/", "`.` with the root as the current directory");
        expect_root("../../..", "/Sys", "`..` past the root stays at it");
    }

    /* A volume or a binding, and the path below it. */
    {
        expect_vfs("/Sys", cwd, "Sys:", "a volume's root");
        expect_vfs("/Sys/", cwd, "Sys:", "a trailing slash names the same root");
        expect_vfs("/AEGIR", cwd, "AEGIR:", "a volume name as written");
        expect_vfs("/aegir/AEGIR.TXT", cwd, "aegir:AEGIR.TXT",
                   "the first component is kept as written (the namespace folds it)");
        expect_vfs("/Sys/AEGIR.TXT", cwd, "Sys:AEGIR.TXT", "an assign and a file below it");
        expect_vfs("/Sys/DOCS/NESTED.TXT", cwd, "Sys:DOCS/NESTED.TXT",
                   "two components deep");
        expect_vfs("/Sys//DOCS", cwd, "Sys:DOCS", "an empty component is a separator");
        expect_vfs("/Sys/./DOCS/", cwd, "Sys:DOCS", "`.` is dropped");
        expect_vfs("/Sys/DOCS/../AEGIR.TXT", cwd, "Sys:AEGIR.TXT", "`..` is resolved");
        expect_vfs("/Sys/DOCS/.", cwd, "Sys:DOCS", "`/.` at the end names the directory");
        expect_vfs("/Sys/a.b", cwd, "Sys:a.b", "a dot inside a name is part of the name");
        expect_vfs("/Sys/..hidden", cwd, "Sys:..hidden",
                   "`..hidden` is a name, not a parent");
    }

    /* A relative path, which the current directory answers for. */
    {
        expect_vfs("AEGIR.TXT", cwd, "Sys:Tests/AEGIR.TXT",
                   "a relative path joins the current directory");
        expect_vfs("./AEGIR.TXT", cwd, "Sys:Tests/AEGIR.TXT", "`./` joins it too");
        expect_vfs("../DOCS/NESTED.TXT", cwd, "Sys:DOCS/NESTED.TXT",
                   "`..` leaves the current directory");
        expect_vfs("..", cwd, "Sys:", "`..` to the volume's root");
        expect_root("../..", cwd, "`..` past the volume's root is the root");
        expect_vfs("DOCS/../AEGIR.TXT", "/Sys", "Sys:AEGIR.TXT",
                   "`.` and `..` in a composed path");
        expect_vfs("AEGIR.TXT", "/", "AEGIR.TXT:",
                   "with the root as the current directory, a name is a volume's root");
    }

    /* A native path, which the view does not touch. */
    {
        expect_vfs("Sys:DOCS/AEGIR.TXT", cwd, "Sys:DOCS/AEGIR.TXT",
                   "a native path passes through");
        expect_vfs("AEGIR:AEGIR.TXT", cwd, "AEGIR:AEGIR.TXT", "a native volume path");
        expect_vfs("Sys:", cwd, "Sys:", "a native volume root");
        expect_vfs("Sys:DOCS/../AEGIR.TXT", cwd, "Sys:DOCS/../AEGIR.TXT",
                   "a native path is not normalized");
        expect_vfs("NIL:", nullptr, "NIL:", "a native path needs no current directory");
    }

    /* What is refused. */
    {
        expect_refused("", cwd, kRoom, "an empty path");
        expect_refused("AEGIR.TXT", nullptr, kRoom, "a relative path with no current directory");
        expect_refused("AEGIR.TXT", "", kRoom, "a relative path with an empty current directory");
        expect_refused("/Sys/AEGIR.TXT", cwd, 4, "a path longer than the room it was given");
    }

    /* The reverse: what a listing and `getcwd` present. */
    {
        expect_present("Sys:Tests", "/Sys/Tests", "a volume path as a POSIX path");
        expect_present("Sys:", "/Sys", "a volume root as a POSIX path");
        expect_present("AEGIR:DOCS/NESTED.TXT", "/AEGIR/DOCS/NESTED.TXT",
                       "two components deep");
        expect_present_refused("nocolon", kRoom, "a path that names no volume");
        expect_present_refused(":foo", kRoom, "a colon with no name before it");
        expect_present_refused("Sys:Tests", 5, "a path longer than the room it was given");
    }

    /* And the two together: a path in, the same path out. */
    {
        char got[kRoom];
        Translation const out = translate("/Sys/DOCS/../DOCS", cwd, got, kRoom);
        char back[kRoom];
        uint32_t back_length = 0;
        bool const presented =
            out.target == Target::Vfs &&
            aegir::posix::path::present(got, out.length, back, kRoom, back_length);
        report(presented && same(back, back_length, "/Sys/DOCS", 9),
               "a translation presents back as the path it came from");
    }

    std::printf("posix path: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
