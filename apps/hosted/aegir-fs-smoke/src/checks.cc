/*
 * aegir-fs-smoke: the hosted wrapper's checks -- implementation.
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * This translation unit is the libc++ side and must not include any seL4
 * header (see checks.h). It exercises aegir::filesystem, the hosted wrapper
 * over the freestanding aegir::vfs transport, with std::filesystem's error
 * model: an error_code overload that reports, and a throwing one.
 */

#include "checks.h"

#include <aegir/debug.h>
#include <aegir/filesystem.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace aegir::fs_smoke {

namespace {

int g_failed = 0;

void report(bool ok, char const *what)
{
    aegir::debug_write(ok ? "  fs-smoke: ok: " : "  fs-smoke: FAIL: ");
    aegir::debug_write(what);
    aegir::debug_write("\n");
    if (!ok) {
        ++g_failed;
    }
}

void check_volumes()
{
    std::error_code error;
    std::vector<aegir::filesystem::VolumeInfo> listed = aegir::filesystem::volumes(error);
    bool ok = !error && !listed.empty();
    for (aegir::filesystem::VolumeInfo const &volume : listed) {
        ok = ok && !volume.name.empty();
    }
    report(ok, "aegir::filesystem::volumes() names the volumes");

    bool threw = false;
    std::size_t throwing_count = 0;
    try {
        throwing_count = aegir::filesystem::volumes().size();
    } catch (std::exception const &) {
        threw = true;
    }
    report(!threw && throwing_count == listed.size(),
           "the throwing volumes() agrees with the error_code one");
}

/* std::filesystem over the runtime's file calls. Every call here lands in
 * musl (stat/open/readdir/mkdir/remove/getcwd) and so in the runtime's file
 * layer (aegir-heap/src/files.cc), which answers it from aegir::vfs. The
 * paths are Aegir's -- absolute, "Volume:rest" -- so no mapping or current
 * directory is involved except where current_path names it. */
void check_std_filesystem()
{
    namespace fs = std::filesystem;

    {
        std::error_code error;
        fs::path const manifest("Initrd:services.manifest");
        bool const is_file = fs::is_regular_file(manifest, error) && !error;
        std::uintmax_t size = 0;
        if (!error) {
            size = fs::file_size(manifest, error);
        }
        report(is_file && !error && size > 0,
               "std::filesystem reads Initrd:services.manifest's status and size");
    }

    {
        std::error_code error;
        bool const is_dir = fs::is_directory(fs::path("Initrd:"), error) && !error;
        report(is_dir, "std::filesystem sees the Initrd: root as a directory");
    }

    {
        std::error_code error;
        bool found = false;
        fs::directory_iterator const end;
        for (fs::directory_iterator it("Initrd:", error); !error && it != end;
             it.increment(error)) {
            if (it->path().filename() == "services.manifest") {
                found = true;
            }
        }
        report(!error && found, "std::filesystem::directory_iterator walks Initrd:");
    }

    {
        std::error_code error;
        fs::path const directory("SCRATCH:SMOKEDIR");
        bool const made = fs::create_directory(directory, error) && !error;
        bool const now_dir = fs::is_directory(directory, error) && !error;
        bool const removed = fs::remove(directory, error) && !error;
        report(made && now_dir && removed,
               "std::filesystem creates and removes a directory on SCRATCH:");
    }

    {
        /* A long name (VFAT, specs/fat.md) through std::filesystem: status,
         * size, and a directory walk that must show the long form whole, not
         * the generated 8.3 alias. Sys: is AEGIR:, the system volume. */
        static char const kExpected[] =
            "a long name, read back whole -- the 8.3 form cannot spell it\n";
        std::error_code error;
        fs::path const named("Sys:Readme With A Long Name.txt");
        bool const is_file = fs::is_regular_file(named, error) && !error;
        std::uintmax_t size = 0;
        if (!error) {
            size = fs::file_size(named, error);
        }
        report(is_file && !error && size == sizeof(kExpected) - 1,
               "std::filesystem reads a long-named file's status and size");
    }

    {
        std::error_code error;
        bool found = false;
        fs::directory_iterator const end;
        for (fs::directory_iterator it("Sys:", error); !error && it != end;
             it.increment(error)) {
            if (it->path().filename() == "A Long Folder") {
                found = true;
            }
        }
        report(!error && found,
               "std::filesystem::directory_iterator shows a long name whole");
    }

    {
        /* A long name created through the runtime's own file calls (libc++ is
         * built without the iostreams, so POSIX open/write/read is the way in):
         * the runtime opens it, the FAT driver writes its run, and the bytes
         * come back whole. */
        static char const kText[] =
            "written through the runtime, read back the same\n";
        static char const kPath[] = "SCRATCH:Created With A Long Name.txt";
        int const fd = ::open(kPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        bool ok = fd >= 0;
        if (ok) {
            ok = ::write(fd, kText, sizeof(kText) - 1) ==
                 static_cast<ssize_t>(sizeof(kText) - 1);
            ::close(fd);
        }
        char buffer[128] = {};
        int const rd = ok ? ::open(kPath, O_RDONLY) : -1;
        ok = ok && rd >= 0;
        if (ok) {
            ssize_t const got = ::read(rd, buffer, sizeof(buffer));
            ok = got == static_cast<ssize_t>(sizeof(kText) - 1);
            for (std::size_t i = 0; ok && i < sizeof(kText) - 1; ++i) {
                ok = buffer[i] == kText[i];
            }
            ::close(rd);
        }
        std::error_code error;
        bool const removed = fs::remove(kPath, error) && !error;
        report(ok && removed,
               "a long-named file created through the runtime reads back whole");
    }

    {
        std::error_code error;
        fs::path const cwd = fs::current_path(error);
        report(!error && !cwd.native().empty(),
               "std::filesystem::current_path reports the process's directory");
    }

    {
        /* The tracked Aegir path grammar (specs/cxx.md step 5): a root name is
         * a volume, and such a path is absolute. A relative path joins the
         * current directory the same way. */
        fs::path const rooted("Initrd:services.manifest");
        bool const grammar = rooted.is_absolute() &&
                             rooted.root_name() == "Initrd:" &&
                             rooted.relative_path() == "services.manifest" &&
                             fs::absolute(rooted) == rooted;
        fs::path const joined = fs::absolute(fs::path("thing"));
        bool const relative = joined.is_absolute() && joined.filename() == "thing";
        report(grammar && relative,
               "std::filesystem gives a path the Aegir grammar (Volume: root, absolute)");
    }
}

}  // namespace

int run()
{
    check_volumes();
    check_std_filesystem();
    return g_failed;
}

}  // namespace aegir::fs_smoke
