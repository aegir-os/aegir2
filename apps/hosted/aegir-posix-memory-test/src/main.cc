/*
 * aegir-posix-memory-test: the memory sub-arc's acceptance client (specs/posix.md).
 *
 * Copyright (c) 2026 Robert Roland
 * SPDX-License-Identifier: MIT
 *
 * A plain program in the POSIX sense: mmap, munmap and the calls that set them
 * up -- open, close, memcmp -- and nothing else, so it carries no Aegir call of
 * its own. The runtime stands it up before `main` (aegir-crt0's constructor),
 * and its evidence is a marker it prints itself: `printf` is a libc call, not
 * the runtime's diagnostic writer, and the terminal mirrors a command's output
 * to the serial the acceptance's runner reads.
 *
 * What it proves is the memory half of the surface that
 * `specs/clang-on-aegir.md` calls the load-bearing gap: a *file's* bytes through
 * a mapping (what LLVM's MemoryBuffer does for every input object file), an
 * anonymous mapping written through and read back, `mprotect` changing a page's
 * protection and leaving the mapping usable, `munmap` returning it, a mapping
 * that follows landing on memory that is there and zero-filled, and MAP_FIXED
 * refused rather than quietly landing somewhere else.
 *
 * The frame behind each mapped page is what `mprotect` needs in hand, and the
 * heap keeps that record in chunks it maps as it needs them -- sizing it from the
 * arena at init was tried and cost the launcher the untyped it spawns commands
 * with (measured: "spawn: FAIL no untyped for the command's runtime").
 *
 * The file is the AEGIR volume's own, whose bytes the path view's client also
 * knows (scripts/make_disk.py's AEGIR_BFS_TREE), and it is read through the view
 * like any other path -- so this client measures memory and the path view at
 * once, without either knowing the other exists.
 *
 * Statuses:
 *    0  every check passed (and it says AEGIR_POSIX_MEMORY_OK on stdout as well)
 *  2+n  the n-th check failed (n counts the calls to require(), in source order)
 *
 * The per-check status is what the terminal's own `command exited <n>` line
 * carries to the runner, so a failure names itself there as well as on the
 * marker's channel (specs/posix.md).
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr char const *kSource = "/AEGIR/AEGIR.TXT";
char const *const kAegirText = "aegir read this file off a disk it enumerated itself\n";
constexpr size_t kPage = 4096;
constexpr size_t kAnonymous = 3 * kPage;

/* Which check is running, so a failure can say so with a status rather than a
 * message (the header). */
int check = 0;

[[noreturn]] void fail(int status, char const *what)
{
    printf("AEGIR_POSIX_MEMORY_FAIL %s (check %d)\n", what, status - 2);
    (void)fflush(nullptr);
    _exit(status);
}

void require(bool ok, char const *what)
{
    ++check;
    if (!ok) {
        fail(2 + check, what);
    }
}

}  // namespace

int main()
{
    /* A file's bytes through a mapping: the whole of what a compiler's
     * MemoryBuffer asks, and the mapping is private and read-only to the caller,
     * so the volume's own bytes cannot change. */
    int const fd = open(kSource, O_RDONLY);
    require(fd >= 0, "open /AEGIR/AEGIR.TXT");
    size_t const length = strlen(kAegirText);
    void *mapped = mmap(nullptr, kPage, PROT_READ, MAP_PRIVATE, fd, 0);
    require(mapped != MAP_FAILED, "mmap the file");
    require(memcmp(mapped, kAegirText, length) == 0, "the mapping holds the file's bytes");
    require(static_cast<char const *>(mapped)[length] == '\0',
            "the rest of the page reads as zero");
    require(munmap(mapped, kPage) == 0, "munmap the file");
    require(close(fd) == 0, "close the file");

    /* An anonymous mapping, written through and read back: the pages a program
     * gets for its own work are zero-filled, and they are writable. */
    char *arena = static_cast<char *>(
        mmap(nullptr, kAnonymous, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(arena != MAP_FAILED, "mmap anonymous read/write");
    require(arena[0] == 0 && arena[kAnonymous - 1] == 0, "a fresh mapping reads as zero");
    for (size_t i = 0; i < kAnonymous; ++i) {
        arena[i] = static_cast<char>('a' + (i % 26));
    }
    require(arena[kAnonymous - 1] == static_cast<char>('a' + ((kAnonymous - 1) % 26)),
            "a written mapping reads back its own bytes");

    /* mprotect: a mapping's protection can be changed and the mapping stays
     * usable -- read-only here, then writable again. What the rights mean is the
     * hardware's business, so this checks what a program relies on: the pages keep
     * what they held, and the mapping takes writes again once it is writable. */
    require(mprotect(arena, kPage, PROT_READ) == 0, "mprotect a page read-only");
    require(arena[0] == 'a', "a read-only mapping still reads its bytes");
    require(mprotect(arena, kPage, PROT_READ | PROT_WRITE) == 0, "mprotect it writable again");
    arena[0] = 'z';
    require(arena[0] == 'z', "and it takes a write again");

    /* munmap gives it back, and a mapping that follows lands on memory that is
     * there and zero-filled -- which is what a real munmap owes a program that
     * turns over large allocations. */
    require(munmap(arena, kAnonymous) == 0, "munmap the anonymous mapping");
    char *again = static_cast<char *>(
        mmap(nullptr, kAnonymous, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    require(again != MAP_FAILED, "mmap again after the release");
    require(again[0] == 0 && again[kAnonymous - 1] == 0, "the mapping that follows is zeroed");
    require(munmap(again, kAnonymous) == 0, "munmap the second mapping");

    /* MAP_FIXED is refused, and the refusal is a boundary worth pinning: the
     * arena's free area is the one interval between the break and its cursor, so
     * a hole punched into it could not be handed back, and a live mapping cannot
     * be replaced (specs/posix.md, specs/memory.md). A program that asks for an
     * address it was never given gets MAP_FAILED rather than a mapping that
     * lands somewhere else. */
    void *const refused = mmap(reinterpret_cast<void *>(kPage), kPage, PROT_READ,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    require(refused == MAP_FAILED, "MAP_FIXED at an address we were not given is refused");

    printf("AEGIR_POSIX_MEMORY_OK\n");
    (void)fflush(nullptr);
    _exit(0);
}
