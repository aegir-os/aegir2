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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <system_error>
#include <sys/xattr.h>
#include <time.h>
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
        /* rename through std::filesystem: musl's rename is renameat2, the
         * runtime answers it, and the FAT driver remakes the entry under the
         * new name with its data unmoved. */
        static char const kText[] = "renamed, not recopied\n";
        static char const kBefore[] = "SCRATCH:Before Rename.txt";
        static char const kAfter[] = "SCRATCH:After Rename.txt";
        int const fd = ::open(kBefore, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        bool ok = fd >= 0;
        if (ok) {
            ok = ::write(fd, kText, sizeof(kText) - 1) ==
                 static_cast<ssize_t>(sizeof(kText) - 1);
            ::close(fd);
        }
        std::error_code error;
        if (ok) {
            fs::rename(kBefore, kAfter, error);
            ok = !error;
        }
        char buffer[64] = {};
        int const rd = ok ? ::open(kAfter, O_RDONLY) : -1;
        ok = ok && rd >= 0;
        if (ok) {
            ssize_t const got = ::read(rd, buffer, sizeof(buffer));
            ok = got == static_cast<ssize_t>(sizeof(kText) - 1);
            for (std::size_t i = 0; ok && i < sizeof(kText) - 1; ++i) {
                ok = buffer[i] == kText[i];
            }
            ::close(rd);
        }
        bool const removed = fs::remove(kAfter, error) && !error;
        report(ok && removed,
               "std::filesystem::rename moves a long-named file, data unmoved");
    }

    {
        /* resize_file (musl's truncate) cuts a file and grows it with zeros;
         * the ftruncate syscall resizes through an open fd. */
        static char const kPath[] = "SCRATCH:Resized.bin";
        char const pattern = 'x';
        int fd = ::open(kPath, O_CREAT | O_TRUNC | O_RDWR, 0644);
        bool ok = fd >= 0 && ::write(fd, &pattern, 1) == 1;
        if (fd >= 0) {
            ::close(fd);
        }
        std::error_code error;
        if (ok) {
            fs::resize_file(kPath, 5, error);
            ok = !error;
        }
        char buffer[8] = {};
        int const rd = ok ? ::open(kPath, O_RDONLY) : -1;
        ok = ok && rd >= 0;
        if (ok) {
            ssize_t const got = ::read(rd, buffer, sizeof(buffer));
            ok = got == 5 && buffer[0] == 'x';
            for (int i = 1; ok && i < 5; ++i) {
                ok = buffer[i] == 0; /* the grown tail is zeros */
            }
            ::close(rd);
        }
        int const rw = ok ? ::open(kPath, O_RDWR) : -1;
        ok = ok && rw >= 0 && ::ftruncate(rw, 1) == 0;
        if (rw >= 0) {
            ::close(rw);
        }
        char one[2] = {};
        int const again = ok ? ::open(kPath, O_RDONLY) : -1;
        ok = ok && again >= 0 &&
             ::read(again, one, sizeof(one)) == 1 && one[0] == 'x';
        if (again >= 0) {
            ::close(again);
        }
        bool const removed = fs::remove(kPath, error) && !error;
        report(ok && removed,
               "std::filesystem::resize_file truncates and grows, ftruncate cuts");
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

/* The clock service (specs/services.md, aegir/clock.h): the runtime answers
 * clock_gettime through the port the manifest gave this process. A plausible
 * wall time is the checksum -- the RTC is QEMU's host clock, so the answer is
 * well past 2020 and well before 2100, and zero or the epoch is a failure. */
void check_clock()
{
    struct timespec realtime = {};
    int const rc = ::clock_gettime(CLOCK_REALTIME, &realtime);
    bool const plausible = rc == 0 && realtime.tv_sec > 1577836800 &&
                           realtime.tv_sec < 4102444800;
    report(plausible, "clock_gettime returns a plausible wall time");

    struct timespec mono = {};
    int const mono_rc = ::clock_gettime(CLOCK_MONOTONIC, &mono);
    report(mono_rc == 0 && mono.tv_sec > 1577836800 && mono.tv_sec < 4102444800,
           "clock_gettime returns a plausible monotonic time");
}

/* The FAT service stamps each entry with the time the clock gave it
 * (specs/fat.md's Times), and the runtime reads the stamp into the kstat's
 * mtime, which is what std::filesystem::last_write_time reads. */
void check_timestamps()
{
    namespace fs = std::filesystem;
    static char const kPath[] = "SCRATCH:Stamped.txt";
    int const fd = ::open(kPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    bool ok = fd >= 0 && ::write(fd, "t", 1) == 1;
    if (fd >= 0) {
        ::close(fd);
    }
    std::error_code error;
    fs::file_time_type written{};
    if (ok) {
        written = fs::last_write_time(kPath, error);
        ok = !error;
    }
    fs::file_time_type const now = fs::file_time_type::clock::now();
    auto const age = now - written;
    ok = ok && age < std::chrono::hours(1) && age > std::chrono::hours(-1);
    bool const removed = fs::remove(kPath, error) && !error;
    report(ok && removed,
           "std::filesystem::last_write_time is the time the file was written");
}

/* The metadata protocol through the POSIX xattr calls (specs/bfs.md). BFS
 * keeps attributes, in the inode or in an attribute inode; FAT has none and
 * the runtime maps the protocol's status word onto errno, so EOPNOTSUPP and
 * ENODATA stay apart. */
void check_attributes()
{
    static char const kPath[] = "BFS:ATTRX.TXT";
    static char const kName[] = "BEOS:TYPE";
    static char const kValue[] = "text/plain";
    static char const kFdName[] = "AEGIR:FDTEST";
    int const fd = ::open(kPath, O_CREAT | O_TRUNC | O_RDWR, 0644);
    bool ok = fd >= 0;
    if (fd >= 0) {
        ok = ::setxattr(kPath, kName, kValue, sizeof(kValue) - 1, 0) == 0;
        ok = ok && ::fsetxattr(fd, kFdName, "viafd", 5, 0) == 0;
        ssize_t const needed = ::getxattr(kPath, kName, nullptr, 0);
        ok = ok && needed == static_cast<ssize_t>(sizeof(kValue) - 1);
        char value[16] = {};
        ssize_t const got = ::getxattr(kPath, kName, value, sizeof(value));
        ok = ok && got == static_cast<ssize_t>(sizeof(kValue) - 1) &&
             std::memcmp(value, kValue, static_cast<size_t>(got)) == 0;
        char from_fd[8] = {};
        ssize_t const fd_got = ::fgetxattr(fd, kFdName, from_fd, sizeof(from_fd));
        ok = ok && fd_got == 5 && std::memcmp(from_fd, "viafd", 5) == 0;
        /* listxattr: the names, each NUL-terminated. */
        ssize_t const list_needed = ::listxattr(kPath, nullptr, 0);
        ok = ok && list_needed > 0;
        if (ok) {
            std::vector<char> list(static_cast<std::size_t>(list_needed));
            ssize_t const listed = ::listxattr(kPath, list.data(), list.size());
            bool saw_type = false;
            bool saw_fd = false;
            ok = listed == list_needed;
            for (std::size_t i = 0; ok && i < list.size();) {
                char const *entry = list.data() + i;
                std::size_t const length = std::strlen(entry);
                if (std::strcmp(entry, kName) == 0) {
                    saw_type = true;
                }
                if (std::strcmp(entry, kFdName) == 0) {
                    saw_fd = true;
                }
                i += length + 1;
            }
            ok = ok && saw_type && saw_fd;
        }
        /* Removing makes it gone: ENODATA, not EOPNOTSUPP. */
        ok = ok && ::removexattr(kPath, kName) == 0;
        errno = 0;
        ssize_t const gone = ::getxattr(kPath, kName, value, sizeof(value));
        ok = ok && gone == -1 && errno == ENODATA;
        (void)::fremovexattr(fd, kFdName);
        ::close(fd);
    }
    ::unlink(kPath);
    report(ok, "POSIX xattr reads, writes, lists and removes BFS attributes");

    /* A filesystem with no attributes says so: EOPNOTSUPP, not ENODATA. */
    static char const kFatPath[] = "SCRATCH:NoAttrs.txt";
    int const fat_fd = ::open(kFatPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fat_fd >= 0) {
        ::close(fat_fd);
    }
    errno = 0;
    ssize_t const unsupported = ::getxattr(kFatPath, kName, nullptr, 0);
    bool const fat_ok = unsupported == -1 && errno == EOPNOTSUPP;
    ::unlink(kFatPath);
    report(fat_ok, "a filesystem with no attributes answers EOPNOTSUPP");
}

}  // namespace

int run()
{
    check_volumes();
    check_std_filesystem();
    check_clock();
    check_timestamps();
    check_attributes();
    return g_failed;
}

}  // namespace aegir::fs_smoke
